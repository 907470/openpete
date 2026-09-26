/*
 * M-key dev config overlay — ImGui panel over the knob registry.
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
 * resolution and never reaches screenshot composes (the caller gates
 * on s_overlay_live). The Vulkan backend rides volk's loaded pointers
 * (IMGUI_IMPL_VULKAN_USE_VOLK); its pipeline is built against off_rp once —
 * swapchain rebuilds recreate off_rp with identical formats, which keeps the
 * pipeline valid (render-pass compatibility), so no re-init on resize/vsync.
 */

#include "imgui.h"
/* The error bracket around a mod UI section body uses the recovery API and
 * the context's error callback, both internal. */
#include "imgui_internal.h"
#include "imgui_impl_sdl3.h"
#include "imgui_impl_vulkan.h"

#include <SDL3/SDL.h>

extern "C" {
#include "../rhi/rhi.h"          /* no C++ guard of its own */
}
#include "openpete.h"
#include "openpete_devui.h"
#include "openpete_fillpick.h"
extern "C" {
#include "openpete_host_input.h"   /* C-only header: the mouse snapshot read */
}
#include "openpete_host_mouse.h"
#include "openpete_knobs.h"
#include "openpete_config.h"
#include "openpete_log.h"
#include "openpete_menu.h"
#include "openpete_mods.h"
#include "openpete_mod_ui.h"

#include <deque>
#include <vector>

#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

static bool        s_inited = false;
static bool        s_open   = false;
/* One present's worth of "reveal": the panel window uncollapsed, the Mods
 * header and every mod node open. Set only when --devui-open opened the
 * overlay, because a run nobody is driving has no way to click a tree node
 * open, and a mod's UI section body runs only inside its node. ImGui
 * does not persist node state, so this is the only way an unattended
 * windowed run reaches a body. */
static bool        s_boot_reveal = false;
static rhi_device *s_dev    = nullptr;
static char        s_ini_path[1024];
/* --devui-inject-mouse X,Y: a cursor position posted into imgui's IO before
 * every NewFrame, so an unattended run can park the pointer over a widget
 * and the hover state can be measured. Off unless both coordinates parse. */
static bool        s_inject_mouse = false;
static float       s_inject_x = 0.0f, s_inject_y = 0.0f;

int openpete_devui_open_requested(const struct openpete_options *opts)
{
    if (opts && opts->devui_open >= 0) return opts->devui_open ? 1 : 0;
    const char *e = getenv("OPENPETE_DEVUI_OPEN");
    return (e && *e && *e != '0') ? 1 : 0;
}

int openpete_devui_inject_mouse_requested(float *x, float *y)
{
    /* Harness-only knob (env var, no CLI spelling): automation parks the
     * cursor so an unattended run can hover a widget. */
    const char *v = getenv("OPENPETE_DEVUI_INJECT_MOUSE");
    if (!v || !*v) return 0;
    char *end = nullptr;
    float px = strtof(v, &end);
    if (!end || *end != ',') return 0;
    char *end2 = nullptr;
    float py = strtof(end + 1, &end2);
    if (!end2 || end2 == end + 1) return 0;
    if (x) *x = px;
    if (y) *y = py;
    return 1;
}

int openpete_devui_init(struct rhi_device *dev, struct SDL_Window *win,
                        const struct openpete_options *opts)
{
    if (s_inited) return 1;
    if (!dev || !win) return 0;

    rhi_imgui_info info;
    if (!rhi_get_imgui_info(dev, &info)) return 0;

    knob_registry_init(opts);
    s_inject_mouse =
        openpete_devui_inject_mouse_requested(&s_inject_x, &s_inject_y)
            != 0;

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
    s_open   = openpete_devui_open_requested(opts) != 0;
    s_boot_reveal = s_open;
    OPL_INFO_F("devui", "dev config overlay ready (M toggles)%s",
               s_open ? " — open at boot (--devui-open)" : "");
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
    s_boot_reveal = false;
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

/* Event queue: the pump and the imgui frame do not necessarily run on the
 * same thread, and imgui is single-threaded — ProcessEvent mutates the same
 * IO state NewFrame consumes. So the pump only *queues* here, and the events
 * are fed to imgui on the thread that records its frame.
 *
 * Single-producer/single-consumer ring. On overflow the newest event is
 * dropped rather than the oldest: losing the tail of a burst degrades an
 * overlay interaction, while dropping the oldest can strip a press and leave
 * imgui holding a key down forever. Drops are counted and reported, because a
 * silently lossy input queue is a miserable thing to debug. */
#define DEVUI_EVQ_CAP 512
static SDL_Event  s_evq[DEVUI_EVQ_CAP];
static SDL_AtomicInt s_evq_head;   /* producer writes */
static SDL_AtomicInt s_evq_tail;   /* consumer writes */
static unsigned   s_evq_dropped;

void openpete_devui_process_event(const union SDL_Event *ev)
{
    if (!s_inited || !ev) return;

    int head = SDL_GetAtomicInt(&s_evq_head);
    int tail = SDL_GetAtomicInt(&s_evq_tail);
    int next = (head + 1) % DEVUI_EVQ_CAP;
    if (next == tail) {
        if ((s_evq_dropped++ % 256u) == 0u)
            OPL_WARN_F("devui", "event queue full; dropped %u", s_evq_dropped);
        return;
    }
    s_evq[head] = *(const SDL_Event *)ev;
    SDL_MemoryBarrierRelease();          /* event visible before the index */
    SDL_SetAtomicInt(&s_evq_head, next);
}

/* Drain queued events into imgui. Must run on the thread that records the
 * imgui frame, immediately before it. */
static void devui_drain_events(void)
{
    int tail = SDL_GetAtomicInt(&s_evq_tail);
    int head = SDL_GetAtomicInt(&s_evq_head);
    SDL_MemoryBarrierAcquire();
    while (tail != head) {
        ImGui_ImplSDL3_ProcessEvent(&s_evq[tail]);
        tail = (tail + 1) % DEVUI_EVQ_CAP;
    }
    SDL_SetAtomicInt(&s_evq_tail, tail);
}

/* imgui's IO capture flags are published rather than read live: they belong to
 * the imgui frame's thread, but the pump needs them too (to decide whether a
 * double-click is the overlay's or the window's). One frame stale by
 * construction, which is what a capture flag is anyway — it describes the
 * frame that was drawn. */
static SDL_AtomicInt s_wants_kb;
static SDL_AtomicInt s_wants_text;
static SDL_AtomicInt s_wants_mouse;

static void devui_publish_io_wants(void)
{
    const ImGuiIO &io = ImGui::GetIO();
    SDL_SetAtomicInt(&s_wants_kb,
                     (s_open && (io.WantCaptureKeyboard || io.WantTextInput)) ? 1 : 0);
    /* Text input is published on its own because the two wants call for
     * different gating: a focused field must swallow every key, while bare
     * nav focus -- which keyboard navigation sets for the whole panel -- must
     * still leave the key that closes the panel readable. */
    SDL_SetAtomicInt(&s_wants_text, (s_open && io.WantTextInput) ? 1 : 0);
    /* The fill-tag inspector draws its interactive panel with the overlay
     * closed, so its hover counts: a click aimed at its buttons must not
     * also reach the game or the settings menu under it. */
    const bool mouse_frame = s_open || openpete_fillpick_active() != 0;
    SDL_SetAtomicInt(&s_wants_mouse, (mouse_frame && io.WantCaptureMouse) ? 1 : 0);
}

/** @brief Publish all three imgui capture wants (keyboard, text input, mouse) as 0.
 *
 * Called on each early return of openpete_devui_render that skips
 * devui_publish_io_wants, so no want from an earlier drawn present stays
 * set: a stale mouse want would keep the overlay as the mouse's owner until
 * the next present that draws it. */
static void devui_publish_none(void)
{
    SDL_SetAtomicInt(&s_wants_kb, 0);
    SDL_SetAtomicInt(&s_wants_text, 0);
    SDL_SetAtomicInt(&s_wants_mouse, 0);
}

int openpete_devui_wants_keyboard(void)
{
    if (!s_inited || !s_open) return 0;
    return SDL_GetAtomicInt(&s_wants_kb);
}

int openpete_devui_wants_text_input(void)
{
    if (!s_inited || !s_open) return 0;
    return SDL_GetAtomicInt(&s_wants_text);
}

int openpete_devui_wants_mouse(void)
{
    if (!s_inited) return 0;
    if (!s_open && !openpete_fillpick_active()) return 0;
    return SDL_GetAtomicInt(&s_wants_mouse);
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

/* A float knob as a slider over its registered range, applied on every
 * change. A drag widget that commits only on release reads as dead while it
 * is moving, and its per-pixel speed suits one range and crawls on another;
 * a slider spans the range whatever the range is. Ctrl+click is imgui's own
 * typed entry and applies through the same call. A row carrying a
 * zero_label has a mode at 0 that the span up to num_min does not reach: a
 * drag through that span only ever hands the setter values it refuses, so
 * the mode is a checkbox and the slider starts at num_min. */
static void knob_row_float(knob_desc_t *k)
{
    double live = k->get_num ? k->get_num(k) : k->stored_num;
    bool off = false;
    if (k->zero_label) {
        off = (live <= 0.0);
        if (ImGui::Checkbox(k->zero_label, &off) && k->set_num) {
            const double v = off ? 0.0 : (double)k->num_min;
            if (k->set_num(k, v) == 0) {
                OPL_INFO_F("devui", "%s = %.3f", k->id, v);
                live = v;
            }
        }
        ImGui::SameLine();
        ImGui::SetNextItemWidth(-FLT_MIN);
        if (off) ImGui::BeginDisabled();
    }
    float v = (float)live;
    if (ImGui::SliderFloat("##v", &v, k->num_min, k->num_max, "%.2f") &&
        k->set_num) {
        if (k->set_num(k, v) == 0)
            OPL_INFO_F("devui", "%s = %.3f", k->id, v);
    }
    if (off) ImGui::EndDisabled();
}

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
    case KNOB_FLOAT:
        knob_row_float(k);
        break;
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
                          "- rows in that position are marked 'pinned by ...'.",
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

static int mui_overlay_open(void) { return s_open ? 1 : 0; }

static void mui_tooltip(const char *text)
{
    if (text && ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip))
        ImGui::SetTooltip("%s", text);
}

static const openpete_mod_ui_t s_mod_ui = {
    mui_text,       mui_text_disabled, mui_button,    mui_checkbox,
    mui_slider_int, mui_slider_float,  mui_combo,     mui_input_text,
    mui_separator,  mui_same_line,     mui_tooltip,   mui_overlay_open,
    {},
};

static void mod_row_tooltip(const openpete_mods_menu_info_t *mi)
{
    if (!ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip)) return;
    ImGui::BeginTooltip();
    ImGui::PushTextWrapPos(ImGui::GetFontSize() * 30.0f);
    ImGui::Text("%s v%s", mi->id, mi->version);
    if (!mi->sim_mutative)
        ImGui::TextDisabled("writes no guest RAM (outside the save "
                            "modset tag)");
    if (!mi->entered)
        ImGui::TextDisabled("not started yet - enabling runs the mod at "
                            "the next tick boundary");
    if (mi->has_assets)
        ImGui::TextDisabled("asset overlay: textures/music follow the "
                            "toggle; level data applies at the next "
                            "level load");
    if (mi->enabled && openpete_mods_registry_owner_busy(mi->id))
        ImGui::TextDisabled("can't be turned off while you're inside "
                            "its level");
    if (mi->enabled && !mi->has_ui && !mi->has_cfg)
        ImGui::TextDisabled("no controls registered");
    if (mi->imgui_calls)
        ImGui::TextDisabled("ImGui: %llu calls last present",
                            (unsigned long long)mi->imgui_calls);
    ImGui::PopTextWrapPos();
    ImGui::EndTooltip();
}

/* ── Engine-rendered config schema ([[config]] rows) ────────────────
 * The engine draws each mod's settings from its manifest schema; edits
 * buffer here (per mod id) and commit through openpete_mods_cfg_apply,
 * which writes config.toml and queues a reload so the mod's entry
 * re-reads the values. Nothing applies until the user hits Apply — a
 * half-dragged slider never half-configures a mod. */

struct mod_cfg_edit_value {
    int64_t i;
    double  d;
    char    s[128];
};

struct mod_cfg_edit_state {
    char id[64];
    int  n;
    bool seeded;
    std::vector<mod_cfg_edit_value> v;   /* one per [[config]] row */
};

/* Deque, not vector: a slot pointer handed to the draw below must stay
 * valid when a later mod's panel appends its own slot. */
static std::deque<mod_cfg_edit_state> s_cfg_edit;

static mod_cfg_edit_state *cfg_edit_slot(const char *id)
{
    for (auto &e : s_cfg_edit)
        if (strcmp(e.id, id) == 0) return &e;
    s_cfg_edit.emplace_back();
    mod_cfg_edit_state *slot = &s_cfg_edit.back();
    snprintf(slot->id, sizeof slot->id, "%s", id);
    slot->n = 0;
    slot->seeded = false;
    return slot;
}

static void cfg_seed(mod_cfg_edit_state *st, int mod_index, int rows)
{
    st->n = rows;
    st->v.assign((size_t)(rows > 0 ? rows : 0), mod_cfg_edit_value{});
    for (int r = 0; r < rows; r++) {
        openpete_mods_cfg_row_t row;
        if (!openpete_mods_cfg_row(mod_index, r, &row)) continue;
        st->v[r].i = row.ival;
        st->v[r].d = row.fval;
        snprintf(st->v[r].s, sizeof st->v[r].s, "%s", row.sval);
    }
    st->seeded = true;
}

static void draw_mod_config_rows(int mod_index,
                                 const openpete_mods_menu_info_t *mi)
{
    const int rows = openpete_mods_cfg_row_count(mod_index);
    if (rows <= 0) return;
    mod_cfg_edit_state *st = cfg_edit_slot(mi->id);
    if (!st) return;
    if (!st->seeded || st->n != rows) cfg_seed(st, mod_index, rows);

    bool dirty = false;
    const char *open_group = "";
    for (int r = 0; r < rows && r < (int)st->v.size(); r++) {
        openpete_mods_cfg_row_t row;
        if (!openpete_mods_cfg_row(mod_index, r, &row)) continue;
        if (row.group[0] && strcmp(row.group, open_group) != 0) {
            ImGui::SeparatorText(row.group);
            open_group = row.group;
        }
        ImGui::PushID(row.key);
        switch (row.type) {
        case 'b': {
            bool b = st->v[r].i != 0;
            if (ImGui::Checkbox(row.label, &b)) st->v[r].i = b ? 1 : 0;
            dirty |= (st->v[r].i != row.ival);
            break;
        }
        case 'i': {
            int iv = (int)st->v[r].i;
            if (row.min < row.max) {
                if (ImGui::SliderInt(row.label, &iv, (int)row.min,
                                     (int)row.max))
                    st->v[r].i = iv;
            } else if (ImGui::InputInt(row.label, &iv)) {
                st->v[r].i = iv;
            }
            dirty |= (st->v[r].i != row.ival);
            break;
        }
        case 'd': {
            float f = (float)st->v[r].d;
            if (row.min < row.max) {
                if (ImGui::SliderFloat(row.label, &f, (float)row.min,
                                       (float)row.max))
                    st->v[r].d = f;
            } else if (ImGui::InputFloat(row.label, &f)) {
                st->v[r].d = f;
            }
            dirty |= (st->v[r].d != row.fval);
            break;
        }
        case 's': {
            if (row.items && row.n_items > 0) {
                int idx = 0;
                for (int k = 0; k < row.n_items; k++)
                    if (strcmp(row.items[k], st->v[r].s) == 0) idx = k;
                if (ImGui::Combo(row.label, &idx, row.items, row.n_items))
                    snprintf(st->v[r].s, sizeof st->v[r].s, "%s",
                             row.items[idx]);
            } else {
                ImGui::InputText(row.label, st->v[r].s,
                                 sizeof st->v[r].s);
            }
            dirty |= (strcmp(st->v[r].s, row.sval) != 0);
            break;
        }
        }
        if (row.help[0] &&
            ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip))
            ImGui::SetTooltip("%s", row.help);
        ImGui::PopID();
    }

    if (dirty) {
        if (ImGui::Button("Apply")) {
            std::vector<openpete_mods_cfg_edit_t> edits(
                (size_t)(rows > 0 ? rows : 0));
            int n = 0;
            for (int r = 0; r < rows && r < (int)st->v.size(); r++) {
                openpete_mods_cfg_row_t row;
                if (!openpete_mods_cfg_row(mod_index, r, &row)) continue;
                edits[n].key  = row.key;
                edits[n].type = row.type;
                edits[n].ival = st->v[r].i;
                edits[n].fval = st->v[r].d;
                edits[n].sval = st->v[r].s;
                n++;
            }
            if (openpete_mods_cfg_apply(mod_index, n, edits.data()) == 0)
                OPL_INFO_F("devui", "mod %s: config applied", mi->id);
        }
        ImGui::SameLine();
        if (ImGui::Button("Revert")) cfg_seed(st, mod_index, rows);
        ImGui::SameLine();
        ImGui::TextDisabled("(pending)");
    }
}

/* ── Mod UI section bracket ──────────────────────────────────────────
 * A body is mod C driving the engine's ImGui context directly (the mod table,
 * openpete_imgui.h). ImGui reports unbalanced Begin/End, Push/Pop and
 * similar misuse as recoverable user errors; the bracket stores the stack
 * sizes before the body and recovers to them after, so a mod that leaves a
 * window open does not swallow the panel or the frame. The body also holds
 * a writable ImGuiStyle and ImGuiIO, so both are snapshotted and put back.
 * ImGui's own reaction is silenced: the assert is inert under NDEBUG and
 * would abort the process in linux-debug, and the tooltip would paint over
 * the panel, so the engine logs the error instead. A body that errors on
 * three consecutive presents loses its section for the session.
 *
 * io.FontGlobalScale is not in the snapshot: it is compiled out under
 * IMGUI_DISABLE_OBSOLETE_FUNCTIONS, and its replacement style.FontScaleMain
 * rides the style copy. */
static bool s_mod_body_errored = false;

static void mod_body_error_cb(ImGuiContext *, void *user_data,
                              const char *msg)
{
    s_mod_body_errored = true;
    OPL_ERROR_F("mods", "%s: ImGui error inside the mod's UI section: "
                "%s", (const char *)user_data, msg ? msg : "");
}

/* Consecutive presents whose body errored, per pass and mod index. Indices
 * are fixed for the session (the loader loads its whole set at boot); an
 * index past the array is simply not counted, so the loader's own cap does
 * not have to be mirrored here.
 *
 * Per pass, because the two passes run in the same present and only one of
 * them runs a given mod's body: for an always section the node pass draws
 * ui_status lines and nothing else, so a shared counter would see a clean
 * bracket from that pass every present and reset a streak the always pass
 * was accumulating — the refusal would never fire. */
static std::vector<int> s_mod_body_err_streak[2];

/* @p always picks the pass: 0 draws the mod's node contents (status lines
 * plus a plain body), 1 draws an always-flagged body at top level. @p
 * no_input is 1 on an always pass with the overlay closed, which puts
 * ImGuiWindowFlags_NoInputs on every window the body opens. */
static void draw_mod_section_bracketed(int mod_index, const char *mod_id,
                                       bool always, bool no_input)
{
    ImGuiContext &g  = *ImGui::GetCurrentContext();
    ImGuiIO      &io = ImGui::GetIO();

    ImGuiErrorRecoveryState recovery;
    ImGui::ErrorRecoveryStoreState(&recovery);

    const ImGuiStyle       style_save    = g.Style;
    const char *const      ini_save      = io.IniFilename;
    const char *const      log_save      = io.LogFilename;
    const ImGuiConfigFlags flags_save    = io.ConfigFlags;
    void *const            user_save     = io.UserData;
    void *const            renderer_save = io.BackendRendererUserData;
    void *const            platform_save = io.BackendPlatformUserData;
    const bool             assert_save   = io.ConfigErrorRecoveryEnableAssert;
    const bool             tooltip_save  = io.ConfigErrorRecoveryEnableTooltip;
    ImGuiErrorCallback     cb_save       = g.ErrorCallback;
    void *const            cb_user_save  = g.ErrorCallbackUserData;

    io.ConfigErrorRecoveryEnableAssert  = false;
    io.ConfigErrorRecoveryEnableTooltip = false;
    g.ErrorCallback         = mod_body_error_cb;
    g.ErrorCallbackUserData = (void *)mod_id;
    s_mod_body_errored      = false;

    if (always)
        openpete_mods_ui_draw_always(mod_index, &s_mod_ui, no_input ? 1 : 0);
    else
        openpete_mods_ui_draw(mod_index, &s_mod_ui);

    ImGui::ErrorRecoveryTryToRecoverState(&recovery);

    g.Style                             = style_save;
    io.IniFilename                      = ini_save;
    io.LogFilename                      = log_save;
    io.ConfigFlags                      = flags_save;
    io.UserData                         = user_save;
    io.BackendRendererUserData          = renderer_save;
    io.BackendPlatformUserData          = platform_save;
    io.ConfigErrorRecoveryEnableAssert  = assert_save;
    io.ConfigErrorRecoveryEnableTooltip = tooltip_save;
    g.ErrorCallback                     = cb_save;
    g.ErrorCallbackUserData             = cb_user_save;

    const int pass = always ? 1 : 0;
    if (mod_index < 0) return;
    /* One counter per loaded mod: the table grows with the mod set, so a
     * late mod's misbehaving body is refused like any other's. */
    if ((size_t)mod_index >= s_mod_body_err_streak[pass].size())
        s_mod_body_err_streak[pass].resize((size_t)mod_index + 1, 0);
    if (!s_mod_body_errored) {
        s_mod_body_err_streak[pass][mod_index] = 0;
        return;
    }
    if (++s_mod_body_err_streak[pass][mod_index] >= 3)
        openpete_mods_ui_refuse_section(
            mod_index, "its body reported an ImGui error on three "
                       "consecutive presents");
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

    if (filter.IsActive() || s_boot_reveal) ImGui::SetNextItemOpen(true);
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
        if (mi.enabled && (mi.has_ui || mi.has_cfg || has_levels)) {
            if (s_boot_reveal) ImGui::SetNextItemOpen(true);
            if (ImGui::TreeNodeEx(mi.name,
                                  ImGuiTreeNodeFlags_SpanAvailWidth)) {
                mod_row_tooltip(&mi);
                draw_mod_config_rows(i, &mi);
                if (mi.has_ui)
                    draw_mod_section_bracketed(i, mi.id, false, false);
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
            ImGui::Text("Custom World %d - donor level %d", 7 + i / 5,
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

/* A row the run has no use for (see knob_desc_t::visible) is left out of the
 * panel entirely — including from the "does this section have anything to
 * show" scan, so its header goes with it. */
static bool row_visible(const knob_desc_t *k)
{
    return !k->visible || k->visible(k) != 0;
}

/* Does this row appear in the panel at all: relevant to the run, then
 * matching the text filter. */
static bool row_shown(const ImGuiTextFilter &f, const knob_desc_t *k)
{
    if (!row_visible(k)) return false;
    if (!f.IsActive()) return true;
    return f.PassFilter(k->id) || f.PassFilter(k->label) ||
           (k->cli && f.PassFilter(k->cli)) || (k->env && f.PassFilter(k->env));
}

static void draw_panel(void)
{
    ImGuiIO &io = ImGui::GetIO();
    ImGui::SetNextWindowPos(ImVec2(24, 24), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(540, io.DisplaySize.y - 96), ImGuiCond_FirstUseEver);
    if (s_boot_reveal) ImGui::SetNextWindowCollapsed(false, ImGuiCond_Always);

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
            if (row_shown(s_filter, &ks[end]) &&
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
                    if (!row_shown(s_filter, k)) continue;
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
    s_boot_reveal = false;
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

/* ------------------------------------------------------------------ */
/* Always-on mod sections                                             */
/* ------------------------------------------------------------------ */

/* Bodies flagged OPENPETE_MOD_UI_ALWAYS, drawn at top level on every
 * present. Top level and not the Mods panel's tree node: a node the user
 * collapsed must not make a mod's window disappear, and the panel does not
 * exist at all while the overlay is closed.
 *
 * Skipped while the settings menu is open. The menu and the overlay are
 * mutually exclusive input owners, and both compose branches draw the menu
 * before this frame is recorded, so an always window would paint over it. */
static void draw_always_sections(void)
{
    if (openpete_menu_is_open()) return;
    const int n = openpete_mods_count();
    for (int i = 0; i < n; i++) {
        openpete_mods_menu_info_t mi;
        if (!openpete_mods_menu_info(i, &mi)) continue;
        if (!mi.enabled || !mi.ui_always) continue;
        ImGui::PushID(mi.id);
        draw_mod_section_bracketed(i, mi.id, true, !s_open);
        ImGui::PopID();
    }
}

int openpete_devui_frame_wanted(void)
{
    if (!s_inited) return 0;
    if (s_open) return 1;
    if (openpete_fillpick_active()) return 1;
    return openpete_mods_ui_always_count() > 0 ? 1 : 0;
}

void openpete_devui_render(struct rhi_device *dev)
{
    /* The inspector's crosshair + panel ride the same imgui frame as the
     * config overlay, but must show with the overlay closed (aiming a camera
     * behind a 540px panel is not the workflow). */
    if (!s_inited) return;

    /* Feed imgui every frame, open or not: its IO bookkeeping (key/mouse
     * state, focus) must stay current or the overlay opens holding whatever
     * was down when it last drew. Draining only while open would also let the
     * queue fill and start dropping. */
    devui_drain_events();

    bool fillpick = openpete_fillpick_active() != 0;
    const int always_n = openpete_mods_ui_always_count();
    if (!s_open && !fillpick && always_n == 0) { devui_publish_none(); return; }
    if (dev != s_dev) { devui_publish_none(); return; }
    void *cb = rhi_current_command_buffer(dev);
    if (!cb) { devui_publish_none(); return; }

    /* First open: adopt late-settling boot values (render-fps pacer). An
     * always section runs the frame with the overlay closed, so the stamp
     * waits for the open that gives it its meaning. */
    if (s_open) {
        static bool s_late_stamped = false;
        if (!s_late_stamped) { knob_registry_late_stamp(); s_late_stamped = true; }
    }

    ImGuiIO &io = ImGui::GetIO();
    /* The SDL3 backend re-applies the cursor shape and calls SDL_ShowCursor
     * on every NewFrame unless this flag is set. With an always section the
     * frame runs every present, which would undo the host loop's idle
     * cursor hide. Decided per present from the open state rather than on
     * its edges, so the first frame that draws the panel is the frame that
     * gets imgui's cursors back. */
    if (s_open) io.ConfigFlags &= ~ImGuiConfigFlags_NoMouseCursorChange;
    else        io.ConfigFlags |=  ImGuiConfigFlags_NoMouseCursorChange;

    ImGui_ImplVulkan_NewFrame();
    ImGui_ImplSDL3_NewFrame();
    /* After the backend's own IO update, which would otherwise overwrite
     * the parked position with the real pointer. */
    if (s_inject_mouse) {
        io.AddMousePosEvent(s_inject_x, s_inject_y);
    } else if (openpete_host_mouse_inject_active()) {
        /* A scripted run: the backend just read SDL's pointer, which a
         * pushed event never moves; follow the channel's event-derived
         * position so the script's hover reaches these widgets. */
        openpete_mouse_state_t st;
        openpete_host_input_mouse(&st);
        io.AddMousePosEvent(st.px, st.py);
    } else if (!openpete_host_mouse_accepting()) {
        /* The backend's NewFrame polls the global pointer position when no
         * window of ours is hovered, which bypasses the event stream the
         * mouse switch and the input lock filter. Report the pointer as
         * absent so nothing under it hovers or activates. */
        io.AddMousePosEvent(-FLT_MAX, -FLT_MAX);
    }
    ImGui::NewFrame();
    if (s_open) draw_panel();
    if (always_n > 0) draw_always_sections();
    if (fillpick) { draw_fillpick_crosshair(); draw_fillpick_panel(); }
    ImGui::Render();
    devui_publish_io_wants();
    ImGui_ImplVulkan_RenderDrawData(ImGui::GetDrawData(), (VkCommandBuffer)cb);
}
