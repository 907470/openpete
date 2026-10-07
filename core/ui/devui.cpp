/*
 * M-key dev config overlay - ImGui panel over the knob registry.
 *
 * One window, sections as collapsing headers laid out by a table that
 * regroups and combines the registry's rows, a search filter, per-row source
 * tag (cli/env/default + edited + the winning openpete.toml layer), tooltips
 * carrying each knob's help text and flag spellings, a Save bar that writes
 * the session's edits into the config file's managed [saved] block, and a
 * right-click context menu (reset to boot / copy as CLI flag / copy as env
 * line - the copy items move a setting to ANOTHER machine, or across the
 * WSL->Windows boundary env vars don't cross).
 *
 * Rendering: records into the offscreen pass (off_rp) at the live-present
 * compose point, after the SSAA resolve - so the panel is always at 1x window
 * resolution and never reaches screenshot composes (the caller gates
 * on s_overlay_live). The Vulkan backend rides volk's loaded pointers
 * (IMGUI_IMPL_VULKAN_USE_VOLK); its pipeline is built against off_rp once -
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
static SDL_Window *s_win    = nullptr;
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
    /* The backend's IME hook calls SDL_StartTextInput from the thread that
     * renders the frame, which is not the thread that owns the window;
     * openpete_devui_pump_text_input does it on the pump thread instead. The
     * backend's per-NewFrame IME update then sees no request and makes no
     * SDL call that changes state. */
    ImGui::GetPlatformIO().Platform_SetImeDataFn = nullptr;

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
    s_win    = win;
    s_inited = true;
    s_open   = openpete_devui_open_requested(opts) != 0;
    s_boot_reveal = s_open;
    OPL_INFO_F("devui", "dev config overlay ready (M toggles)%s",
               s_open ? " - open at boot (--devui-open)" : "");
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
    s_win    = nullptr;
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
 * same thread, and imgui is single-threaded - ProcessEvent mutates the same
 * IO state NewFrame consumes. So the pump only *queues* here, and the events
 * are fed to imgui on the thread that records its frame.
 *
 * A text-input event's `text` points into SDL's temporary memory, which SDL
 * frees at the start of the next SDL_PollEvent, before the frame thread
 * drains the ring. The text is copied into the slot's own buffer and the
 * event repointed at it; text longer than a slot spans consecutive slots,
 * split on UTF-8 character boundaries.
 *
 * Single-producer/single-consumer ring. On overflow the newest event is
 * dropped rather than the oldest: losing the tail of a burst degrades an
 * overlay interaction, while dropping the oldest can strip a press and leave
 * imgui holding a key down forever. Drops are counted and reported, because a
 * silently lossy input queue is a miserable thing to debug. */
#define DEVUI_EVQ_CAP 512
#define DEVUI_EVQ_TEXT 64
static SDL_Event  s_evq[DEVUI_EVQ_CAP];
static char       s_evq_text[DEVUI_EVQ_CAP][DEVUI_EVQ_TEXT];
static SDL_AtomicInt s_evq_head;   /* producer writes */
static SDL_AtomicInt s_evq_tail;   /* consumer writes */
static unsigned   s_evq_dropped;

/** @brief Append one event to the ring, or count it dropped when the ring is full.
 *
 * A text-input event's text is copied into the slot's buffer, at most
 * DEVUI_EVQ_TEXT - 1 bytes cut on a character boundary.
 * @param ev The event; for text input, `text` is the chunk to store.
 * @return Bytes of text stored for a text-input event, 1 for any other
 *         stored event, 0 when dropped. */
static size_t devui_enqueue(const SDL_Event *ev)
{
    int head = SDL_GetAtomicInt(&s_evq_head);
    int tail = SDL_GetAtomicInt(&s_evq_tail);
    int next = (head + 1) % DEVUI_EVQ_CAP;
    if (next == tail) {
        if ((s_evq_dropped++ % 256u) == 0u)
            OPL_WARN_F("devui", "event queue full; dropped %u", s_evq_dropped);
        return 0;
    }
    size_t stored = 1;
    s_evq[head] = *ev;
    if (ev->type == SDL_EVENT_TEXT_INPUT) {
        stored = SDL_utf8strlcpy(s_evq_text[head], ev->text.text,
                                 DEVUI_EVQ_TEXT);
        s_evq[head].text.text = s_evq_text[head];
    }
    SDL_MemoryBarrierRelease();          /* event visible before the index */
    SDL_SetAtomicInt(&s_evq_head, next);
    return stored;
}

void openpete_devui_process_event(const union SDL_Event *ev)
{
    if (!s_inited || !ev) return;
    const SDL_Event *e = (const SDL_Event *)ev;
    if (e->type != SDL_EVENT_TEXT_INPUT) {
        (void)devui_enqueue(e);
        return;
    }
    if (!e->text.text) return;
    SDL_Event chunk = *e;
    const char *rest = e->text.text;
    while (*rest) {
        chunk.text.text = rest;
        const size_t n = devui_enqueue(&chunk);
        if (n == 0) return;
        rest += n;
    }
}

void openpete_devui_pump_text_input(void)
{
    static bool s_text_active = false;
    if (!s_inited || !s_win) return;
    const bool want = openpete_devui_wants_text_input() != 0;
    if (want == s_text_active) return;
    if (want) SDL_StartTextInput(s_win);
    else      SDL_StopTextInput(s_win);
    s_text_active = want;
}

/** @brief Feed every queued SDL event to imgui's SDL3 backend and advance the ring's tail.
 *
 * Runs on the thread that records the imgui frame, immediately before it. */
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
 * construction, which is what a capture flag is anyway - it describes the
 * frame that was drawn. */
static SDL_AtomicInt s_wants_kb;
static SDL_AtomicInt s_wants_text;
static SDL_AtomicInt s_wants_mouse;

/** @brief Publish imgui's keyboard, text-input and mouse capture wants for the pump's threads.
 *
 * Called after ImGui::Render. The keyboard and text wants are 0 while the
 * panel is closed; the mouse want also counts while the fill-tag inspector
 * is active. */
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

/** @brief Name the source of a knob's boot value for the panel's source column.
 *
 * KNOB_SRC_CLI reads "cli", KNOB_SRC_ENV "env", and every other value
 * (KNOB_SRC_DEFAULT) "def". The column shows "edited" instead while the
 * row's value differs from its boot value.
 * @param s The source recorded at boot.
 * @return A static three-letter tag. */
static const char *src_tag(knob_src_t s)
{
    switch (s) {
    case KNOB_SRC_CLI: return "cli";
    case KNOB_SRC_ENV: return "env";
    default:           return "def";
    }
}

/** @brief Format a knob's current value as its CLI or environment spelling, for the clipboard copy.
 *
 * A bool reads 0 or 1; every other type uses knob_value_str.
 * @param k The knob.
 * @param buf Receives the text.
 * @param cap The capacity of buf. */
static void format_value_token(const knob_desc_t *k, char *buf, size_t cap)
{
    if (k->type == KNOB_BOOL) {
        double v = k->get_num ? k->get_num(k) : k->stored_num;
        snprintf(buf, cap, "%d", v != 0.0 ? 1 : 0);
    } else {
        knob_value_str(k, buf, cap);
    }
}

/** @brief Report whether a knob's current value differs from its boot value.
 *
 * String and action rows never count as edited. */
static bool row_edited(const knob_desc_t *k)
{
    if (k->type == KNOB_STRING) return false;   /* strings are read-only v1 */
    if (k->type == KNOB_ACTION) return false;   /* fires; holds no value */
    double v = k->get_num ? k->get_num(k) : k->stored_num;
    return v != k->boot_num;
}

/** @brief Draw a knob row's right-click menu: reset to boot value and clipboard copies.
 *
 * Reset appears only for a live, settable, non-action knob and calls its
 * setter with the boot value. The copy items appear when the knob has a CLI
 * spelling or an environment variable name, and put "<flag> <value>" or
 * "<var>=<value>" on the clipboard. */
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

/** @brief Draw the hover tooltip of the item just submitted: a knob's help and spellings.
 *
 * Shows over a disabled item too. Adds a "restart to apply" line for a
 * restart-only knob. */
static void row_tooltip(const knob_desc_t *k)
{
    if (!ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip |
                              ImGuiHoveredFlags_AllowWhenDisabled))
        return;
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

/** @brief Draw a float knob as a slider over its registered range, applied on every change.
 *
 * A drag widget that commits only on release reads as dead while it is
 * moving, and its per-pixel speed suits one range and crawls on another; a
 * slider spans the range whatever the range is. Ctrl+click is imgui's own
 * typed entry and applies through the same call. A row carrying a
 * zero_label has a mode at 0 that the span up to num_min does not reach: a
 * drag through that span only ever hands the setter values it refuses, so
 * the mode is a checkbox and the slider starts at num_min.
 * @param k The knob. */
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

/** @brief Read a numeric knob's current value: its getter, else the registry's stored copy. */
static double knob_num(const knob_desc_t *k)
{
    return k->get_num ? k->get_num(k) : k->stored_num;
}

/** @brief Draw a bool knob as a checkbox applied through its setter on each click. */
static void knob_widget_bool(knob_desc_t *k)
{
    bool b = knob_num(k) != 0.0;
    if (ImGui::Checkbox("##v", &b) && k->set_num) {
        if (k->set_num(k, b ? 1.0 : 0.0) == 0)
            OPL_INFO_F("devui", "%s = %s", k->id, b ? "on" : "off");
    }
}

/** @brief Draw an int knob as an input field that commits when it loses focus after an edit.
 *
 * A half-typed value must not reach a setter with an apply cost (rewind
 * clears its history), so the edit lives in the shadow until release, and
 * the committed value is clamped to the knob's range when the range is
 * non-empty. */
static void knob_widget_int(knob_desc_t *k)
{
    int v = (int)((s_edit_row == k) ? s_edit_val : knob_num(k));
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
}

/** @brief Draw an enum knob as a combo box applied through its setter on each pick. */
static void knob_widget_enum(knob_desc_t *k)
{
    int idx = (int)knob_num(k);
    if (ImGui::Combo("##v", &idx, k->enum_values, k->enum_count) && k->set_num) {
        if (k->set_num(k, idx) == 0)
            OPL_INFO_F("devui", "%s = %s", k->id,
                       (idx >= 0 && idx < k->enum_count) ? k->enum_values[idx] : "?");
    }
}

/** @brief Draw an action knob as a button that fires its setter with 1.0, followed by its status text.
 *
 * get_num, when present, gates the button: 0 draws it disabled. */
static void knob_widget_action(knob_desc_t *k)
{
    const bool ready = !k->get_num || k->get_num(k) != 0.0;
    if (!ready) ImGui::BeginDisabled();
    if (ImGui::Button(k->action_label ? k->action_label : "Apply") && k->set_num) {
        if (k->set_num(k, 1.0) == 0)
            OPL_INFO_F("devui", "%s fired", k->id);
    }
    if (!ready) ImGui::EndDisabled();
    if (!k->get_str) return;
    char status[KNOB_STR_MAX];
    k->get_str(k, status, sizeof status);
    if (!status[0]) return;
    ImGui::SameLine();
    ImGui::AlignTextToFramePadding();
    ImGui::TextDisabled("%s", status);
}

/** @brief Draw a string knob as a text field, editable and committed on release only for a live row with a setter.
 *
 * imgui keeps its own edit state while the field is active and writes back
 * into the buffer each frame, so a per-frame buffer seeded from the live
 * value is safe. */
static void knob_widget_string(knob_desc_t *k)
{
    char buf[KNOB_STR_MAX];
    knob_value_str(k, buf, sizeof buf);
    if (!k->set_str || k->mut != KNOB_LIVE) {
        ImGui::InputText("##v", buf, sizeof buf, ImGuiInputTextFlags_ReadOnly);
        return;
    }
    ImGui::InputText("##v", buf, sizeof buf);
    if (ImGui::IsItemDeactivatedAfterEdit() && k->set_str(k, buf) == 0)
        OPL_INFO_F("devui", "%s = %s", k->id, buf);
}

/** @brief Draw a knob's type-specific value widget, disabled unless the knob is live, with its tooltip.
 *
 * The caller sets the item width. */
static void knob_widget(knob_desc_t *k)
{
    const bool disabled = (k->mut != KNOB_LIVE);
    if (disabled) ImGui::BeginDisabled();
    switch (k->type) {
    case KNOB_BOOL:   knob_widget_bool(k);   break;
    case KNOB_INT:    knob_widget_int(k);    break;
    case KNOB_FLOAT:  knob_row_float(k);     break;
    case KNOB_ENUM:   knob_widget_enum(k);   break;
    case KNOB_ACTION: knob_widget_action(k); break;
    case KNOB_STRING: knob_widget_string(k); break;
    }
    if (disabled) ImGui::EndDisabled();
    row_tooltip(k);
}

/** @brief Draw the label cell of a panel row: the caption, with the primary knob's tooltip and right-click menu.
 *
 * @param label The caption.
 * @param k The row's primary knob. */
static void row_label_cell(const char *label, knob_desc_t *k)
{
    ImGui::TableNextColumn();
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(label);
    row_tooltip(k);
    row_context_menu(k);
}

/** @brief Draw the source cell of a panel row over its knobs.
 *
 * Reads "edited" when any knob's value differs from its boot value, else
 * the strongest boot source among them (cli over env over default). The
 * first knob a launch tier pins adds "pinned" with the pinning spelling in
 * its tooltip, else the first non-default config layer is shown in brackets.
 * A restart-only row says so in its disabled widget and its tooltip.
 * @param ks The row's knobs, the primary first.
 * @param n The knob count. */
static void row_source_cell(knob_desc_t *const *ks, int n)
{
    ImGui::TableNextColumn();
    bool edited = false;
    knob_src_t src = KNOB_SRC_DEFAULT;
    for (int i = 0; i < n; i++) {
        edited = edited || row_edited(ks[i]);
        if (ks[i]->boot_src > src) src = ks[i]->boot_src;
    }
    if (edited)
        ImGui::TextColored(ImVec4(1.0f, 0.75f, 0.30f, 1.0f), "edited");
    else
        ImGui::TextDisabled("%s", src_tag(src));

    /* A launch-tier override outranks anything in the file, so saving this
     * row would look like it did nothing next launch. Say so on the row
     * rather than letting the user discover it by restarting. */
    for (int i = 0; i < n; i++) {
        const char *pin = openpete_config_knob_pin_spelling(ks[i]->id);
        if (!pin) continue;
        ImGui::SameLine();
        ImGui::TextColored(ImVec4(0.55f, 0.75f, 1.0f, 1.0f), "pinned");
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip))
            ImGui::SetTooltip("%s is set for this launch, which outranks\n"
                              "openpete.toml. A saved value applies once that\n"
                              "override is gone.", pin);
        return;
    }
    for (int i = 0; i < n; i++) {
        const char *layer = openpete_config_knob_layer(ks[i]->id);
        if (!layer || strcmp(layer, "default") == 0) continue;
        ImGui::SameLine();
        ImGui::TextDisabled("[%s]", layer);
        return;
    }
}

/** @brief Draw one knob as a table row: label, type-specific value widget, source column. */
static void draw_row(knob_desc_t *k)
{
    ImGui::TableNextRow();
    row_label_cell(k->label, k);
    ImGui::TableNextColumn();
    ImGui::SetNextItemWidth(-FLT_MIN);
    knob_widget(k);
    row_source_cell(&k, 1);
}

/* ------------------------------------------------------------------ */
/* Save bar                                                           */
/* ------------------------------------------------------------------ */

/** @brief Draw the Save settings and Reset saved buttons and the last status line.
 *
 * Save writes every row edited this session into the config file's [saved]
 * block and is disabled while no row is edited; Reset saved empties that
 * block. Persisting is explicit and never happens on close, so the config
 * file changes only on a click. */
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

/* The SDK's openpete_mod_ui_t widget vtable, implemented over imgui -
 * mods draw their section through these and never see imgui types.
 * Callbacks run inside the mod's section under the render-purity guard
 * (openpete_mods_ui_draw for the tree node, openpete_mods_ui_draw_always
 * for a top-level always section). */

/** @brief Draw printf-formatted text, truncated to 1023 bytes.
 *
 * @param fmt The printf format, followed by its arguments. */
static void mui_text(const char *fmt, ...)
{
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    ImGui::TextUnformatted(buf);
}

/** @brief Draw printf-formatted text in the disabled colour, truncated to 1023 bytes. */
static void mui_text_disabled(const char *fmt, ...)
{
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    ImGui::TextDisabled("%s", buf);
}

/** @brief Draw a button and report whether it was clicked (1) or not (0). */
static int mui_button(const char *label)
{
    return ImGui::Button(label) ? 1 : 0;
}

/** @brief Draw a checkbox over an int and report 1 when the click changed it.
 *
 * A NULL v draws an unchecked box and stores nothing. */
static int mui_checkbox(const char *label, int *v)
{
    bool b = v && *v != 0;
    if (ImGui::Checkbox(label, &b)) {
        if (v) *v = b ? 1 : 0;
        return 1;
    }
    return 0;
}

/** @brief Draw an int slider over [mn, mx] and report 1 when the value changed; a NULL v draws nothing. */
static int mui_slider_int(const char *label, int *v, int mn, int mx)
{
    return (v && ImGui::SliderInt(label, v, mn, mx)) ? 1 : 0;
}

/** @brief Draw a float slider over [mn, mx] and report 1 when the value changed; a NULL v draws nothing. */
static int mui_slider_float(const char *label, float *v, float mn, float mx)
{
    return (v && ImGui::SliderFloat(label, v, mn, mx)) ? 1 : 0;
}

/** @brief Draw a combo box over the item strings and report 1 when the selection changed.
 *
 * A NULL idx or items draws nothing. */
static int mui_combo(const char *label, int *idx, const char *const *items,
                     int count)
{
    return (idx && items && ImGui::Combo(label, idx, items, count)) ? 1 : 0;
}

/** @brief Draw a text field editing buf of cap bytes in place and report 1 when the text changed.
 *
 * A NULL buf or a non-positive cap draws nothing. */
static int mui_input_text(const char *label, char *buf, int cap)
{
    if (!buf || cap <= 0) return 0;
    return ImGui::InputText(label, buf, (size_t)cap) ? 1 : 0;
}

/** @brief Draw a horizontal separator line. */
static void mui_separator(void) { ImGui::Separator(); }
/** @brief Place the next widget on the previous widget's line. */
static void mui_same_line(void) { ImGui::SameLine(); }

/** @brief Report 1 while the config panel is open, else 0. */
static int mui_overlay_open(void) { return s_open ? 1 : 0; }

/** @brief Show text as a tooltip while the previous widget is hovered; a NULL text shows nothing. */
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

/** @brief Draw the hover tooltip of a mod's row: id and version plus the state notes that apply.
 *
 * Notes cover a mod that writes no guest RAM, one not yet started, an asset
 * overlay, an enabled mod that cannot be disabled while its level is
 * active, an enabled mod with no controls, and last present's ImGui call
 * count. */
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
 * re-reads the values. Nothing applies until the user hits Apply - a
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

/** @brief Find the config edit buffer of a mod id, appending an unseeded one when none exists.
 *
 * The returned pointer stays valid for the session because the slots live
 * in a deque. */
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

/** @brief Fill a mod's edit buffer from the schema's current values, one entry per config row.
 *
 * Rows the registry cannot return stay zeroed. Marks the buffer seeded with
 * the given row count. */
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

/** @brief Draw a mod's [[config]] rows over its edit buffer, with Apply and Revert once edited.
 *
 * The buffer is reseeded when it is new or the row count changed. Rows are
 * grouped under separator headings by their group field; a bool, int, float
 * or string row uses a checkbox, an int or float slider (when min < max, else
 * a plain input), or a combo (string with an item list) or text field. Apply
 * hands every row to openpete_mods_cfg_apply; Revert reseeds from the
 * current values. Nothing reaches the mod before Apply. */
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

/** @brief Log an ImGui error raised inside a mod's UI body and flag that the body errored.
 *
 * Installed as the context's error callback for the duration of the body.
 * user_data is the mod id. */
static void mod_body_error_cb(ImGuiContext *, void *user_data,
                              const char *msg)
{
    s_mod_body_errored = true;
    OPL_ERROR_F("mods", "%s: ImGui error inside the mod's UI section: "
                "%s", (const char *)user_data, msg ? msg : "");
}

/* Consecutive presents whose body errored, per pass and mod index. The
 * vector grows to the highest mod index drawn, so a mod loaded after boot is
 * counted like any other.
 *
 * Per pass, because the two passes run in the same present and only one of
 * them runs a given mod's body: for an always section the node pass draws
 * ui_status lines and nothing else, so a shared counter would see a clean
 * bracket from that pass every present and reset a streak the always pass
 * was accumulating - the refusal would never fire. */
static std::vector<int> s_mod_body_err_streak[2];

/** @brief Draw one mod's UI section inside the error-recovery bracket and count its errors.
 *
 * Stores ImGui's stack state, style and writable IO fields before the body
 * and restores them after; routes ImGui errors to mod_body_error_cb. A body
 * that errors on three consecutive presents of the same pass has its
 * section refused for the session.
 * @param mod_index The loader's mod index; a negative index skips the
 *        error count.
 * @param mod_id The mod id, named in the error log.
 * @param always false draws the mod's node contents (status lines plus a
 *        plain body); true draws an always-flagged body at top level.
 * @param no_input true on an always pass with the overlay closed, which
 *        puts ImGuiWindowFlags_NoInputs on every window the body opens. */
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

/** @brief Report whether a mod row passes the text filter, by matching its id or name.
 *
 * An inactive filter passes every row. */
static bool mod_row_matches(const ImGuiTextFilter &f,
                            const openpete_mods_menu_info_t *mi)
{
    if (!f.IsActive()) return true;
    return f.PassFilter(mi->id) || f.PassFilter(mi->name);
}

static bool mod_owns_custom_levels(const char *mod_id);
static void draw_custom_level_rows(const char *mod_id);

/** @brief Draw the Mods header with one row per loaded mod that passes the filter.
 *
 * Draws nothing when no mod passes. Each row has an enable checkbox that
 * queues the toggle (disabled while the mod owns the level being played)
 * and a name; an enabled mod with a UI section, config rows or custom
 * levels gets a tree node holding its config rows, its bracketed UI body and
 * its level rows. An active filter or the boot reveal forces the header
 * open. */
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
         * applied state back - no local shadow to go stale. Grayed while
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

/** @brief Report whether a mod declares any [[level]] custom level.
 *
 * @param mod_id The mod id.
 * @return true when a custom level in the registry names mod_id as owner. */
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

/** @brief Draw a mod's declarative [[level]] rows, each with a Warp button, inside its node in the Mods list.
 *
 * The registry's index order is alphabetical across all mods, and that
 * global order buckets levels five per Custom World (7 + index / 5), so the
 * world number is shown per row in the tooltip rather than as a grouping.
 * @param mod_id The owning mod's id. */
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

/** @brief Report whether a knob row applies to this run.
 *
 * A row whose knob_desc_t::visible callback returns 0 is left out of the
 * panel and out of the scan that decides whether its section has a row to
 * show, so a section whose rows are all hidden emits no header. A NULL
 * callback means always shown.
 * @param k The knob row.
 * @return true when the row applies. */
static bool row_visible(const knob_desc_t *k)
{
    return !k->visible || k->visible(k) != 0;
}

/** @brief Report whether a knob passes the panel's text filter.
 *
 * An inactive filter passes every knob; an active one passes a knob when it
 * matches the knob's id, label, CLI spelling, or environment variable name.
 * @param f The panel's text filter.
 * @param k The knob row.
 * @return true when the knob matches. */
static bool knob_matches(const ImGuiTextFilter &f, const knob_desc_t *k)
{
    if (!f.IsActive()) return true;
    return f.PassFilter(k->id) || f.PassFilter(k->label) ||
           (k->cli && f.PassFilter(k->cli)) || (k->env && f.PassFilter(k->env));
}

/* ------------------------------------------------------------------ */
/* panel layout                                                       */
/* ------------------------------------------------------------------ */

/* The panel's arrangement of the registry. The registry's own section field
 * groups the --help text and the docs map; the panel regroups rows here,
 * combines related knobs into one row, and picks the widget. A registry row
 * that no layout row names and that is not in k_lay_hidden is drawn by a
 * LAY_REST entry naming its registry section, or, when no entry names that
 * section, under a header of the section's own name after the laid-out
 * sections - so a new registry row always appears somewhere. */
enum lay_kind {
    LAY_ROW,       /* ids[0] with its type's widget */
    LAY_SEG,       /* ids[0] (enum or bool) as a row of buttons */
    LAY_INLINE,    /* ids[0..2] side by side; ids[0] at 0 disables the rest */
    LAY_DRAWMODE,  /* ids[0] notex, ids[1] wireframe as one three-way combo */
    LAY_REWIND,    /* ids[0] rewind: on/off box, the window field while on */
    LAY_WARP,      /* ids[0] level combo, ids[1] its action button */
    LAY_REST,      /* every unplaced row of registry section `label` */
    LAY_SUB,       /* opens a subsection titled `label`, closed by default */
    LAY_SUB_END
};

struct lay_row {
    lay_kind           kind;
    const char        *label;    /* caption; LAY_REST: the registry section */
    const char        *ids[3];   /* knob ids; unused slots NULL */
    const char *const *seg;      /* LAY_SEG captions; NULL = the enum's values */
    const double      *seg_val;  /* value each caption sets; NULL = its index */
    int                seg_n;
};

struct lay_section {
    const char    *title;
    bool           open;      /* expanded until the user collapses it */
    bool           mods;      /* the Mods section in place of rows */
    const lay_row *rows;
    size_t         n;
};

#define LAY_N(a) (sizeof(a) / sizeof((a)[0]))

static const char *const k_depth_caps[] = { "15-bit", "24-bit" };
static const double      k_depth_vals[] = { 1.0, 0.0 };

static const lay_row k_lay_display[] = {
    { LAY_ROW,    nullptr, { "fullscreen" } },
    { LAY_ROW,    nullptr, { "aspect" } },
    { LAY_ROW,    nullptr, { "render_fps" } },
    { LAY_ROW,    nullptr, { "fov" } },
    { LAY_ROW,    nullptr, { "ssaa" } },
    { LAY_ROW,    nullptr, { "vsync" } },
    { LAY_INLINE, "Performance stats", { "perf_stats", "perf_position" } },
    { LAY_INLINE, "Notifications", { "osd", "osd_position", "osd_timeout" } },
    { LAY_SEG,    nullptr, { "vertex_precision" } },
    { LAY_ROW,    nullptr, { "raster_quantize" } },
    { LAY_SEG,    nullptr, { "dither" }, k_depth_caps, k_depth_vals,
      (int)LAY_N(k_depth_caps) },
    { LAY_SEG,    nullptr, { "texture_mapping" } },
    { LAY_SEG,    nullptr, { "lod" } },
    { LAY_REST,   "Display" },
    { LAY_SUB,    "Advanced" },
    { LAY_INLINE, "Fog", { "fog", "fog_scale" } },
    { LAY_DRAWMODE, "Render mode", { "notex", "wireframe" } },
    { LAY_ROW,    nullptr, { "terrain_cache" } },
    { LAY_SEG,    nullptr, { "terrain_visibility" } },
    { LAY_ROW,    nullptr, { "version_osd" } },
    { LAY_SUB_END, nullptr },
};

static const lay_row k_lay_audio[] = { { LAY_REST, "Audio" } };
static const lay_row k_lay_input[] = { { LAY_REST, "Input" } };

static const lay_row k_lay_gameplay[] = {
    { LAY_ROW,    nullptr, { "freecam" } },
    { LAY_ROW,    nullptr, { "tick_scale" } },
    { LAY_ROW,    nullptr, { "runahead" } },
    { LAY_REWIND, "Rewind", { "rewind" } },
    { LAY_REST,   "Gameplay" },
    { LAY_WARP,   "Level warp", { "warp_level", "warp_go" } },
};

static const lay_row k_lay_info[] = {
    { LAY_ROW,    nullptr, { "skip_to_level" } },
    { LAY_ROW,    nullptr, { "skip_to_cutscene" } },
    { LAY_ROW,    nullptr, { "skip_to_area" } },
    { LAY_ROW,    nullptr, { "freecam_pose" } },
    { LAY_ROW,    nullptr, { "player_pose" } },
    { LAY_ROW,    nullptr, { "bk2" } },
    { LAY_ROW,    nullptr, { "bk2_frame_offset" } },
    { LAY_ROW,    nullptr, { "bk2_record" } },
    { LAY_ROW,    nullptr, { "renderer" } },
    { LAY_ROW,    nullptr, { "extract_threads" } },
    { LAY_ROW,    nullptr, { "window" } },
    { LAY_ROW,    nullptr, { "background" } },
    { LAY_SUB,    "Paths" },
    { LAY_REST,   "Paths" },
    { LAY_SUB_END, nullptr },
};

static const lay_row k_lay_recording[] = { { LAY_REST, "Recording" } };

static const lay_row k_lay_debug[] = {
    { LAY_ROW,    nullptr, { "TERRAIN_HDFOLD" } },
    { LAY_REST,   "Debug" },
};

static const lay_section k_layout[] = {
    { "Display",   true,  false, k_lay_display,   LAY_N(k_lay_display) },
    { "Audio",     false, false, k_lay_audio,     LAY_N(k_lay_audio) },
    { "Input",     false, false, k_lay_input,     LAY_N(k_lay_input) },
    { "Gameplay",  true,  false, k_lay_gameplay,  LAY_N(k_lay_gameplay) },
    { "Info",      false, false, k_lay_info,      LAY_N(k_lay_info) },
    { "Recording", true,  false, k_lay_recording, LAY_N(k_lay_recording) },
    { "Mods",      true,  true,  nullptr,         0 },
    { "Debug",     false, false, k_lay_debug,     LAY_N(k_lay_debug) },
};

/* Registry rows the panel does not show. They stay in the registry, so
 * --help, --config-report, the config file and saving still carry them. */
static const char *const k_lay_hidden[] = {
    "input_lock", "camlock", "bk2_stop_on_end", "cheats",
};

/** @brief Report whether a knob id is named by a layout row or hidden from the panel.
 *
 * @param id The knob id.
 * @return true when a LAY_REST entry must skip the row. */
static bool lay_names_id(const char *id)
{
    for (const char *h : k_lay_hidden)
        if (strcmp(h, id) == 0) return true;
    for (const lay_section &s : k_layout)
        for (size_t r = 0; r < s.n; r++)
            for (const char *rid : s.rows[r].ids)
                if (rid && strcmp(rid, id) == 0) return true;
    return false;
}

/** @brief Report whether a registry section is drawn by a LAY_REST entry of the layout. */
static bool lay_rests_section(const char *section)
{
    for (const lay_section &s : k_layout)
        for (size_t r = 0; r < s.n; r++)
            if (s.rows[r].kind == LAY_REST && strcmp(s.rows[r].label, section) == 0)
                return true;
    return false;
}

/* Per registry row: 1 when the layout names it or hides it. Rebuilt when the
 * row count changes; rows are added only before the panel first draws. */
static std::vector<unsigned char> s_lay_claimed;

/** @brief Refresh the per-row claimed flags when the registry's row count changed. */
static void lay_claim_refresh(const knob_desc_t *ks, size_t n)
{
    if (s_lay_claimed.size() == n) return;
    s_lay_claimed.assign(n, 0);
    for (size_t i = 0; i < n; i++)
        s_lay_claimed[i] = lay_names_id(ks[i].id) ? 1 : 0;
}

/* The registry and the filter for one panel draw. */
struct lay_ctx {
    knob_desc_t           *ks;
    size_t                 n;
    const ImGuiTextFilter *filter;
};

/** @brief Find the visible registry row with an id.
 *
 * @return The row; NULL when no row has the id or the row is hidden for this run. */
static knob_desc_t *lay_find(const lay_ctx &c, const char *id)
{
    if (!id) return nullptr;
    for (size_t i = 0; i < c.n; i++)
        if (strcmp(c.ks[i].id, id) == 0)
            return row_visible(&c.ks[i]) ? &c.ks[i] : nullptr;
    return nullptr;
}

/** @brief Report whether a registry row is drawn by a LAY_REST entry of its section under the filter. */
static bool lay_rest_shows(const lay_ctx &c, size_t i, const char *section)
{
    return !s_lay_claimed[i] && strcmp(c.ks[i].section, section) == 0 &&
           row_visible(&c.ks[i]) && knob_matches(*c.filter, &c.ks[i]);
}

/** @brief Collect a layout row's knobs that exist and apply to this run.
 *
 * @param out Receives up to three knobs, in id order, skipping absent ones.
 * @return The knob count; 0 when the row's first id is absent. */
static int lay_knobs(const lay_ctx &c, const lay_row &r, knob_desc_t *out[3])
{
    int n = 0;
    for (const char *id : r.ids) {
        knob_desc_t *k = lay_find(c, id);
        if (k) out[n++] = k;
        else if (n == 0 && id == r.ids[0]) return 0;
    }
    return n;
}

/** @brief Report whether a layout row draws anything under the filter.
 *
 * A LAY_REST entry shows when any of its rows does; a knob row when its knobs
 * exist (both for LAY_DRAWMODE and LAY_WARP) and its caption or one of its
 * knobs matches. */
static bool lay_row_shows(const lay_ctx &c, const lay_row &r)
{
    if (r.kind == LAY_SUB || r.kind == LAY_SUB_END) return false;
    if (r.kind == LAY_REST) {
        for (size_t i = 0; i < c.n; i++)
            if (lay_rest_shows(c, i, r.label)) return true;
        return false;
    }
    knob_desc_t *ks[3];
    const int n = lay_knobs(c, r, ks);
    const bool pair = (r.kind == LAY_DRAWMODE || r.kind == LAY_WARP);
    if (n == 0 || (pair && n < 2)) return false;
    if (r.label && c.filter->PassFilter(r.label)) return true;
    for (int i = 0; i < n; i++)
        if (knob_matches(*c.filter, ks[i])) return true;
    return false;
}

/** @brief Report whether any row in [from, to) of a section draws under the filter. */
static bool lay_span_shows(const lay_ctx &c, const lay_row *rows, size_t from,
                           size_t to)
{
    for (size_t r = from; r < to; r++)
        if (lay_row_shows(c, rows[r])) return true;
    return false;
}

/** @brief Draw an enum or bool knob as a row of equal-width buttons, the current value's highlighted.
 *
 * Clicking another button applies its value through the knob's setter.
 * @param k The knob.
 * @param r The layout row: its seg captions and values, or the enum's. */
static void seg_widget(knob_desc_t *k, const lay_row &r)
{
    const int n = r.seg ? r.seg_n : k->enum_count;
    const char *const *caps = r.seg ? r.seg : k->enum_values;
    double live = knob_num(k);
    if (k->type == KNOB_BOOL) live = (live != 0.0) ? 1.0 : 0.0;
    const float sp = ImGui::GetStyle().ItemInnerSpacing.x;
    const float w = (ImGui::GetContentRegionAvail().x - sp * (float)(n - 1)) / (float)n;
    const bool disabled = (k->mut != KNOB_LIVE);
    if (disabled) ImGui::BeginDisabled();
    for (int i = 0; i < n; i++) {
        const double val = r.seg_val ? r.seg_val[i] : (double)i;
        const bool on = (live == val);
        if (i > 0) ImGui::SameLine(0.0f, sp);
        if (on) ImGui::PushStyleColor(ImGuiCol_Button,
                                      ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
        ImGui::PushID(i);
        const bool clicked = ImGui::Button(caps[i], ImVec2(w, 0.0f));
        ImGui::PopID();
        if (on) ImGui::PopStyleColor();
        row_tooltip(k);
        if (clicked && !on && k->set_num && k->set_num(k, val) == 0)
            OPL_INFO_F("devui", "%s = %s", k->id, caps[i]);
    }
    if (disabled) ImGui::EndDisabled();
}

/** @brief Draw up to three knobs side by side; while the first reads 0 the others are disabled.
 *
 * A checkbox takes its natural width; the other widgets share the remaining
 * width equally. */
static void inline_widget(knob_desc_t *const *ks, int n)
{
    const bool gate_off = knob_num(ks[0]) == 0.0;
    int flex = 0;
    for (int i = 0; i < n; i++)
        if (ks[i]->type != KNOB_BOOL) flex++;
    const float sp = ImGui::GetStyle().ItemSpacing.x;
    for (int i = 0; i < n; i++) {
        if (i > 0) ImGui::SameLine();
        const bool off = (i > 0 && gate_off);
        if (off) ImGui::BeginDisabled();
        ImGui::PushID(i);
        if (ks[i]->type != KNOB_BOOL) {
            const float avail = ImGui::GetContentRegionAvail().x;
            ImGui::SetNextItemWidth(flex > 1 ? avail / (float)flex - sp : -FLT_MIN);
            flex--;
        }
        knob_widget(ks[i]);
        ImGui::PopID();
        if (off) ImGui::EndDisabled();
    }
}

/** @brief Draw the untextured and wireframe views as one Textured / Untextured / Wireframe combo.
 *
 * Wireframe sets the wireframe knob and clears untextured; Untextured the
 * reverse; Textured clears both.
 * @param notex The untextured-view knob.
 * @param wire The wireframe-view knob. */
static void drawmode_widget(knob_desc_t *notex, knob_desc_t *wire)
{
    static const char *const k_modes[] = { "Textured", "Untextured", "Wireframe" };
    int idx = knob_num(wire) != 0.0 ? 2 : knob_num(notex) != 0.0 ? 1 : 0;
    const bool disabled = notex->mut != KNOB_LIVE || wire->mut != KNOB_LIVE ||
                          !notex->set_num || !wire->set_num;
    if (disabled) ImGui::BeginDisabled();
    ImGui::SetNextItemWidth(-FLT_MIN);
    if (ImGui::Combo("##v", &idx, k_modes, (int)LAY_N(k_modes)) && !disabled) {
        wire->set_num(wire, idx == 2 ? 1.0 : 0.0);
        notex->set_num(notex, idx == 1 ? 1.0 : 0.0);
        OPL_INFO_F("devui", "render mode = %s", k_modes[idx]);
    }
    if (disabled) ImGui::EndDisabled();
    row_tooltip(idx == 2 ? wire : notex);
}

/** @brief Draw the rewind knob as an on/off box, with the window field beside it only while on.
 *
 * Turning it on restores the last window that was on, 30 seconds before any. */
static void rewind_widget(knob_desc_t *k)
{
    static int s_last_on = 30;
    const double live = knob_num(k);
    if (live > 0.0) s_last_on = (int)live;
    bool on = live > 0.0 || s_edit_row == k;
    const bool disabled = (k->mut != KNOB_LIVE || !k->set_num);
    if (disabled) ImGui::BeginDisabled();
    if (ImGui::Checkbox("##on", &on) && !disabled) {
        const double v = on ? (double)s_last_on : 0.0;
        if (k->set_num(k, v) == 0)
            OPL_INFO_F("devui", "%s = %d", k->id, (int)v);
    }
    if (disabled) ImGui::EndDisabled();
    row_tooltip(k);
    if (!on) return;
    ImGui::SameLine();
    ImGui::SetNextItemWidth(-FLT_MIN);
    knob_widget(k);
}

/** @brief Draw a level combo and its action button on one line; the button's status is its tooltip.
 *
 * The combo's list grows to the height of the display before it scrolls.
 * @param lvl The enum knob holding the selected entry.
 * @param go The action knob the button fires. */
static void warp_widget(knob_desc_t *lvl, knob_desc_t *go)
{
    const char *cap = go->action_label ? go->action_label : "Apply";
    const ImGuiStyle &st = ImGui::GetStyle();
    const float bw = ImGui::CalcTextSize(cap).x + st.FramePadding.x * 2.0f;
    ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - bw - st.ItemSpacing.x);
    const int idx = (int)knob_num(lvl);
    const char *preview =
        (idx >= 0 && idx < lvl->enum_count) ? lvl->enum_values[idx] : "";
    if (ImGui::BeginCombo("##lvl", preview, ImGuiComboFlags_HeightLargest)) {
        for (int i = 0; i < lvl->enum_count; i++) {
            const bool sel = (i == idx);
            if (ImGui::Selectable(lvl->enum_values[i], sel) && lvl->set_num &&
                lvl->set_num(lvl, i) == 0)
                OPL_INFO_F("devui", "%s = %s", lvl->id, lvl->enum_values[i]);
            if (sel) ImGui::SetItemDefaultFocus();
        }
        ImGui::EndCombo();
    }
    row_tooltip(lvl);

    ImGui::SameLine();
    const bool ready = !go->get_num || go->get_num(go) != 0.0;
    if (!ready) ImGui::BeginDisabled();
    if (ImGui::Button(cap) && go->set_num && go->set_num(go, 1.0) == 0)
        OPL_INFO_F("devui", "%s fired", go->id);
    if (!ready) ImGui::EndDisabled();
    if (!ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip |
                              ImGuiHoveredFlags_AllowWhenDisabled))
        return;
    char status[KNOB_STR_MAX] = "";
    if (go->get_str) go->get_str(go, status, sizeof status);
    ImGui::BeginTooltip();
    ImGui::PushTextWrapPos(ImGui::GetFontSize() * 30.0f);
    if (status[0]) {
        ImGui::TextUnformatted(status);
        ImGui::Separator();
    }
    ImGui::TextUnformatted(go->help);
    ImGui::PopTextWrapPos();
    ImGui::EndTooltip();
}

/** @brief Draw one layout row of knobs as a table row: caption, widget, source column. */
static void lay_draw_knob_row(const lay_ctx &c, const lay_row &r)
{
    knob_desc_t *ks[3];
    const int n = lay_knobs(c, r, ks);
    if (r.kind == LAY_ROW) { draw_row(ks[0]); return; }
    ImGui::TableNextRow();
    row_label_cell(r.label ? r.label : ks[0]->label, ks[0]);
    ImGui::TableNextColumn();
    switch (r.kind) {
    case LAY_SEG:      seg_widget(ks[0], r);           break;
    case LAY_INLINE:   inline_widget(ks, n);           break;
    case LAY_DRAWMODE: drawmode_widget(ks[0], ks[1]);  break;
    case LAY_REWIND:   rewind_widget(ks[0]);           break;
    case LAY_WARP:     warp_widget(ks[0], ks[1]);      break;
    default:                                           break;
    }
    row_source_cell(ks, n);
}

/** @brief Draw the rows [from, to) of a section, none of them a subsection marker, as one table.
 *
 * A LAY_REST entry draws every unclaimed registry row of its section in
 * registry order. */
static void lay_draw_table(const lay_ctx &c, const lay_row *rows, size_t from,
                           size_t to)
{
    if (!lay_span_shows(c, rows, from, to)) return;
    ImGui::PushID((int)from);
    if (ImGui::BeginTable("##t", 3, ImGuiTableFlags_RowBg |
                                    ImGuiTableFlags_SizingStretchProp)) {
        ImGui::TableSetupColumn("name",   ImGuiTableColumnFlags_WidthStretch, 0.42f);
        ImGui::TableSetupColumn("value",  ImGuiTableColumnFlags_WidthStretch, 0.42f);
        ImGui::TableSetupColumn("source", ImGuiTableColumnFlags_WidthStretch, 0.16f);
        for (size_t r = from; r < to; r++) {
            if (!lay_row_shows(c, rows[r])) continue;
            if (rows[r].kind != LAY_REST) {
                ImGui::PushID((int)r);
                lay_draw_knob_row(c, rows[r]);
                ImGui::PopID();
                continue;
            }
            for (size_t i = 0; i < c.n; i++) {
                if (!lay_rest_shows(c, i, rows[r].label)) continue;
                ImGui::PushID(c.ks[i].id);
                draw_row(&c.ks[i]);
                ImGui::PopID();
            }
        }
        ImGui::EndTable();
    }
    ImGui::PopID();
}

/** @brief Find the index of the LAY_SUB_END that closes the subsection opened at `sub`, or `n` when none does. */
static size_t lay_sub_end(const lay_row *rows, size_t sub, size_t n)
{
    size_t j = sub + 1;
    while (j < n && rows[j].kind != LAY_SUB_END) j++;
    return j;
}

/** @brief Draw a section's rows: tables between subsections, each subsection a tree node closed by default.
 *
 * An active filter opens every subsection with a match; a subsection with no
 * row to show is left out. */
static void lay_draw_rows(const lay_ctx &c, const lay_row *rows, size_t n)
{
    size_t i = 0;
    while (i < n) {
        if (rows[i].kind != LAY_SUB) {
            size_t j = i;
            while (j < n && rows[j].kind != LAY_SUB) j++;
            lay_draw_table(c, rows, i, j);
            i = j;
            continue;
        }
        const size_t end = lay_sub_end(rows, i, n);
        if (lay_span_shows(c, rows, i + 1, end)) {
            if (c.filter->IsActive()) ImGui::SetNextItemOpen(true);
            if (ImGui::TreeNodeEx(rows[i].label, ImGuiTreeNodeFlags_SpanAvailWidth)) {
                lay_draw_table(c, rows, i + 1, end);
                ImGui::TreePop();
            }
        }
        i = end + 1;
    }
}

/** @brief Draw one section under its collapsing header when any of its rows shows.
 *
 * An active filter forces the header open. */
static void lay_draw_section(const lay_ctx &c, const char *title, bool open,
                             const lay_row *rows, size_t n)
{
    if (!lay_span_shows(c, rows, 0, n)) return;
    if (c.filter->IsActive()) ImGui::SetNextItemOpen(true);
    ImGui::PushID(title);
    if (ImGui::CollapsingHeader(title, open ? ImGuiTreeNodeFlags_DefaultOpen : 0))
        lay_draw_rows(c, rows, n);
    ImGui::PopID();
}

/** @brief Draw, after the laid-out sections, each registry section no LAY_REST entry names, under its own name.
 *
 * Sections come in registry order and hold their unclaimed rows; a Debug*
 * section starts collapsed. */
static void lay_draw_unplaced(const lay_ctx &c)
{
    size_t i = 0;
    while (i < c.n) {
        const char *section = c.ks[i].section;
        size_t end = i;
        while (end < c.n && strcmp(c.ks[end].section, section) == 0) end++;
        if (!lay_rests_section(section)) {
            const lay_row rest[] = { { LAY_REST, section } };
            lay_draw_section(c, section, strncmp(section, "Debug", 5) != 0,
                             rest, 1);
        }
        i = end;
    }
}

/** @brief Draw the search field over the full panel width, a magnifier drawn inside its left edge.
 *
 * @param f The panel's filter; rebuilt when the text changes. */
static void draw_search(ImGuiTextFilter &f)
{
    const float icon = ImGui::GetFontSize();
    const ImVec2 pad = ImGui::GetStyle().FramePadding;
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding,
                        ImVec2(pad.x * 2.0f + icon, pad.y));
    ImGui::SetNextItemWidth(-FLT_MIN);
    if (ImGui::InputTextWithHint("##search", "Search settings", f.InputBuf,
                                 IM_COUNTOF(f.InputBuf)))
        f.Build();
    ImGui::PopStyleVar();

    /* A lens and a handle at 45 degrees, in the hint text's colour. */
    const ImVec2 mn = ImGui::GetItemRectMin();
    const ImVec2 mx = ImGui::GetItemRectMax();
    const float r = icon * 0.30f;
    const ImVec2 ctr(mn.x + pad.x + icon * 0.42f, (mn.y + mx.y) * 0.5f - icon * 0.08f);
    const ImU32 col = ImGui::GetColorU32(ImGuiCol_TextDisabled);
    ImDrawList *dl = ImGui::GetWindowDrawList();
    dl->AddCircle(ctr, r, col, 0, 1.5f);
    const float d = r * 0.7071f;
    dl->AddLine(ImVec2(ctr.x + d, ctr.y + d),
                ImVec2(ctr.x + d + r * 0.75f, ctr.y + d + r * 0.75f), col, 2.0f);
}

/** @brief Draw the "openpete config" window: search field, Save bar, the laid-out sections, then any unplaced registry section.
 *
 * Writes the window's close state back to the open flag and clears the boot
 * reveal. */
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
    draw_search(s_filter);
    draw_save_bar();

    lay_ctx c;
    c.ks = knob_registry(&c.n);
    c.filter = &s_filter;
    lay_claim_refresh(c.ks, c.n);

    for (const lay_section &s : k_layout) {
        if (s.mods) draw_mods_section(s_filter);
        else        lay_draw_section(c, s.title, s.open, s.rows, s.n);
    }
    lay_draw_unplaced(c);

    ImGui::End();
    s_open = open;
    s_boot_reveal = false;
}

/* ------------------------------------------------------------------ */
/* LP fill-tag inspector (P) - see openpete_fillpick.h                 */
/* ------------------------------------------------------------------ */

/** @brief Draw the fill-tag inspector's crosshair at the window centre, the pick point.
 *
 * The terrain walk picks at the projection centre (OFX/OFY), which is the
 * optical axis at every aspect, so the window centre is the pick point.
 * Drawn on the foreground list, yellow over a 1-px black shadow, so it sits
 * over the scene without a window. */
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

/** @brief Name a fill-tag verdict: "FILL", "NOFILL", or "clear" for any other value. */
static const char *fp_verdict_word(int v)
{
    return v == OPENPETE_FILLPICK_FILL   ? "FILL"
         : v == OPENPETE_FILLPICK_NOFILL ? "NOFILL"
                                         : "clear";
}

/** @brief Draw the fill-tag inspector window over the picked face and the session verdicts.
 *
 * Shows the level, sector and face under the crosshair, the current verdict
 * and whether it is a session override, and the LP view-Z. When a
 * high-poly face lies on the same sightline it reports whether that face is
 * behind or in front of the LP. The FILL, NOFILL and clear buttons mark the
 * picked face; undo removes the last session verdict; the child list shows
 * the session's verdicts newest first. */
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

/** @brief Draw every enabled mod's OPENPETE_MOD_UI_ALWAYS body at top level on this present.
 *
 * Top level and not the Mods panel's tree node: a node the user collapsed
 * must not make a mod's window disappear, and the panel does not exist at
 * all while the overlay is closed. Skipped while the settings menu is open. The menu and the overlay are
 * mutually exclusive input owners, and both compose branches draw the menu
 * before this frame is recorded, so an always window would paint over it.
 * With the overlay closed the bodies' windows take no input. */
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
