/**
 * @file imgui_panel.c
 * @brief imgui-panel: the Dear ImGui annex driving a real panel.
 *
 * The eleven-widget vtable in openpete_mod_ui.h is the simple path.
 * openpete_imgui.h is the escape hatch: the whole Dear ImGui C API,
 * reached through a table the loader binds at dlopen. Opting in is the
 * OPENPETE_MOD_IMGUI() line below; nothing else changes about how the
 * mod is built or loaded.
 *
 * Division of labour, which is the rule and not a style choice:
 *
 * - The tick hook (an override on GamestateUpdate, which runs every tick
 *   from the title screen on) runs on the sim thread inside the tick. It
 *   is the only place that touches guest RAM: it walks the level moby
 *   array, counts the live mobys into a ring and copies the ten nearest
 *   to Spyro into host memory.
 * - The UI section body runs on the compose thread, inside the engine's
 *   ImGui frame, under the render-purity guard. It reads the host copy
 *   and calls ImGui. A guest read from here would sample the tick at an
 *   arbitrary point and a guest write would not survive a replay.
 *
 * An ImGui call from anywhere but a section body is dropped by the
 * engine's guard wrappers, returns a zeroed value and logs the mod and
 * the function once. The "hook_call" config row provokes exactly that.
 */

#include <openpete_mod_api.h>
#include <openpete_mod_ui.h>
#include <openpete_imgui.h>
#include <openpete_sdk_symbols.h>
#include <openpete_game_structs.h>
#include <psx_runtime.h>

#include <stddef.h>

/** Defines the table pointer every ImGui_* call reads and exports the
 * descriptor the loader resolves. Exactly one source file per mod. */
OPENPETE_MOD_IMGUI()

static const openpete_mod_api_t* g_api;
static openpete_mod_t*           g_self;

/* ── guest layout ──────────────────────────────────────────────────── */

/** Stride of the level moby array. Field offsets come from the typed Moby
 * view in openpete_game_structs.h, so a layout change fails this compile
 * instead of silently reading the wrong bytes. g_Spyro's own moby is the
 * head of the Spyro struct, so the same offsets apply to it. */
#define MOBY_STRIDE ((uint32_t)sizeof(Moby))
#define MOBY_POS    ((uint32_t)offsetof(Moby, m_Position))
#define MOBY_CLASS  ((uint32_t)offsetof(Moby, m_Class))
#define MOBY_STATE  ((uint32_t)offsetof(Moby, m_State))
/** The array ends at the first moby whose m_State is -1. This bounds the
 * walk in case a mid-load array has no terminator yet. */
#define MOBY_SCAN_MAX 4096

/* ── host-side copy the section reads ──────────────────────────────── */

#define RING_N    240   /**< About eight seconds of ticks. */
#define NEAREST_N  10

/** Live moby count, one sample per tick, oldest at s_ring_head. */
static float    s_ring[RING_N];
static int      s_ring_head;
static int      s_ring_filled;

typedef struct {
    int      cls;
    int      x, y, z;
    unsigned dist;   /**< Chebyshev distance to Spyro; ordering only. */
} near_moby_t;

static near_moby_t s_near[NEAREST_N];
static int         s_near_n;
static int         s_live_count;
static unsigned    s_ticks;

/** Marker colour, edited in the panel and read by the draw-list marker.
 * Host-side: a widget writes it on the compose thread, the same thread
 * that reads it back, and no guest byte depends on it. */
static float s_marker_col[3] = { 0.95f, 0.75f, 0.20f };

/* ── config ────────────────────────────────────────────────────────── */

static int s_break_end;   /**< Omit an ImGui_End (exercise the bracket). */
static int s_hook_call;   /**< Call ImGui from a present hook (exercise the guard). */

/* ── tick hook: the only guest reads in this mod ───────────────────── */

static unsigned abs_diff(int a, int b) {
    return (unsigned)(a > b ? a - b : b - a);
}

/** Insert one moby into the nearest-N list, which is kept sorted by
 * distance. N is 10, so an insertion sort beats collecting and sorting. */
static void near_insert(int cls, int x, int y, int z, unsigned dist) {
    int i, j;
    if (s_near_n == NEAREST_N && dist >= s_near[NEAREST_N - 1].dist) return;
    for (i = 0; i < s_near_n && s_near[i].dist <= dist; i++) { }
    if (s_near_n < NEAREST_N) s_near_n++;
    for (j = s_near_n - 1; j > i; j--) s_near[j] = s_near[j - 1];
    s_near[i].cls  = cls;
    s_near[i].x    = x;
    s_near[i].y    = y;
    s_near[i].z    = z;
    s_near[i].dist = dist;
}

static void sample_mobys(void) {
    uint32_t m = op_read_u32(g_api, OP_GADDR_g_LevelMobys);
    const int sx = op_read_s32(g_api, OP_GADDR_g_Spyro + MOBY_POS);
    const int sy = op_read_s32(g_api, OP_GADDR_g_Spyro + MOBY_POS + 4u);
    const int sz = op_read_s32(g_api, OP_GADDR_g_Spyro + MOBY_POS + 8u);
    int n;

    s_near_n = 0;
    s_live_count = 0;
    if (!m) return;

    for (m |= 0x80000000u, n = 0; n < MOBY_SCAN_MAX; n++, m += MOBY_STRIDE) {
        int state = (int)(int8_t)op_read_u8(g_api, m + MOBY_STATE);
        int x, y, z;
        if (state == -1) break;
        s_live_count++;
        x = op_read_s32(g_api, m + MOBY_POS);
        y = op_read_s32(g_api, m + MOBY_POS + 4u);
        z = op_read_s32(g_api, m + MOBY_POS + 8u);
        near_insert((int)(int16_t)op_read_u16(g_api, m + MOBY_CLASS),
                    x, y, z,
                    abs_diff(x, sx) + abs_diff(y, sy) + abs_diff(z, sz));
    }
}

static void on_gamestate_update(CPUState* cpu) {
    if (s_hook_call) {
        /* The same refusal from the sim thread. A different function than
         * the present hook's on purpose: the engine reports each (mod,
         * function) pair once, so one function from both sites would
         * collapse to a single line whenever the two hooks share a thread,
         * and the two dispatch sites could not be told apart. */
        ImGui_TextUnformatted("imgui-panel: nor does this one");
    }
    sample_mobys();
    s_ring[s_ring_head] = (float)s_live_count;
    s_ring_head = (s_ring_head + 1) % RING_N;
    if (s_ring_filled < RING_N) s_ring_filled++;
    if (!s_ticks)
        g_api->log(g_self, OP_MOD_LOG_INFO, "imgui-panel: sampling mobys");
    s_ticks++;
    g_api->base(cpu);
}

/* ── outside the section, so the guard drops the call ──────────────── */

static void on_present(const openpete_present_ctx_t* ctx) {
    (void)ctx;
    /* A present hook runs on the extracting thread, outside the engine's
     * NewFrame/Render pair. The engine drops this and names the mod once;
     * the point of the row is to see that happen. */
    ImGui_Text("imgui-panel: this call never reaches Dear ImGui");
}

/* ── UI section: host copy in, ImGui out ───────────────────────────── */

static void ui_draw(const openpete_mod_ui_t* ui) {
    ImDrawList* fg;
    ImVec2 at;
    int i;

    ui->text("%d live moby(s), %u tick(s) sampled", s_live_count, s_ticks);

    /* The ring is written head-first and wraps, so the plot is handed the
     * head as its offset rather than being rotated into a scratch copy. */
    ImGui_PlotLinesEx("live mobys", s_ring, s_ring_filled,
                      s_ring_filled == RING_N ? s_ring_head : 0,
                      NULL, 0.0f, 260.0f, (ImVec2){ 0.0f, 60.0f },
                      (int)sizeof(float));

    ImGui_SeparatorText("nearest to Spyro");
    if (ImGui_BeginTable("imgui-panel-nearest", 3,
                         ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
                         ImGuiTableFlags_SizingStretchProp)) {
        ImGui_TableSetupColumn("class", 0);
        ImGui_TableSetupColumn("distance", 0);
        ImGui_TableSetupColumn("position", 0);
        ImGui_TableHeadersRow();
        for (i = 0; i < s_near_n; i++) {
            ImGui_TableNextRow();
            ImGui_TableNextColumn();
            ImGui_Text("%d", s_near[i].cls);
            ImGui_TableNextColumn();
            ImGui_Text("%u", s_near[i].dist);
            ImGui_TableNextColumn();
            ImGui_Text("%d, %d, %d", s_near[i].x, s_near[i].y, s_near[i].z);
        }
        ImGui_EndTable();
    }
    if (!s_near_n)
        ui->text_disabled("no level mobys yet");

    /* An ImGui_Begin inside the Mods panel's tree node opens an
     * independent top-level window, which is Dear ImGui's documented
     * behaviour and the reason a body can own a window at all. */
    ImGui_SetNextWindowSize((ImVec2){ 260.0f, 0.0f }, ImGuiCond_FirstUseEver);
    if (ImGui_Begin("imgui-panel marker", NULL, 0)) {
        ImGui_ColorEdit3("marker", s_marker_col, 0);
        ImGui_Text("drawn on the foreground draw list");
        at = ImGui_GetCursorScreenPos();
        fg = ImGui_GetForegroundDrawList();
        ImDrawList_AddCircleEx(fg, (ImVec2){ at.x + 24.0f, at.y + 24.0f },
                               18.0f,
                               IM_COL32((int)(s_marker_col[0] * 255.0f),
                                        (int)(s_marker_col[1] * 255.0f),
                                        (int)(s_marker_col[2] * 255.0f), 255),
                               24, 2.0f);
    }
    /* The whole point of the row: with it on the window is never ended,
     * Dear ImGui reports a recoverable error, and the engine's bracket
     * recovers the stack and refuses this section after three presents. */
    if (!s_break_end)
        ImGui_End();
}
OPENPETE_MOD_UI_SECTION(ui_draw)

int openpete_mod_entry(const openpete_mod_api_t* api, openpete_mod_t* self) {
    g_api = api; g_self = self;
    if (api->api_version < 11u) return -1;

    s_break_end = api->config_bool(self, "break_end", 0);
    s_hook_call = api->config_bool(self, "hook_call", 0);

    api->ui_status(self, "open this section to drive Dear ImGui directly");
    if (s_break_end)
        api->ui_status(self, "break_end: an ImGui_End is omitted on purpose");
    if (s_hook_call) {
        api->ui_status(self, "hook_call: a present hook calls into the annex");
        if (api->register_present_hook(self, on_present) != 0) return -1;
    }
    return api->override_name(self, "GamestateUpdate", on_gamestate_update);
}
