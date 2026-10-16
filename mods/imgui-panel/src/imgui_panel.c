/**
 * @file imgui_panel.c
 * @brief imgui-panel: the full ImGui C API driving a real panel.
 *
 * The eleven-widget vtable in openpete_mod_ui.h is the simple path.
 * openpete_imgui.h is the escape hatch: the whole ImGui C API,
 * reached through a table the loader binds at dlopen. Opting in is the
 * OPENPETE_MOD_IMGUI() line below; nothing else changes about how the
 * mod is built or loaded.
 *
 * Division of labour, which is the rule and not a style choice:
 *
 * - The tick hook (Spyro 1: an override on GamestateUpdate, which runs
 *   every tick from the title screen on; Spyro 2 and 3: the per-tick
 *   camera update, bound by address, which runs on gameplay ticks) runs
 *   on the sim thread inside the tick. It
 *   is the only place that touches guest RAM: it walks the level moby
 *   array, counts the live mobys into a ring and copies the ten nearest
 *   to Spyro into host memory.
 * - The UI section body runs on the compose thread, inside the engine's
 *   ImGui frame, under the render-purity guard. It reads the host copy
 *   and calls ImGui. A guest read from here would sample the tick at an
 *   arbitrary point and a guest write would not survive a replay.
 *
 * The section is registered with OPENPETE_MOD_UI_ALWAYS, so the body runs
 * on every present at top level rather than inside the mod's node of the
 * Mods panel. An always body owns its windows: the stats window and the
 * foreground marker draw whatever the overlay is doing, and the detail
 * window (plot, table, colour editor) is opened only while
 * ui->overlay_open() reports the overlay is up, since a panel nobody can
 * click is clutter over the game. With the overlay closed the engine puts
 * ImGuiWindowFlags_NoInputs on every window the body opens, which the
 * hover counter below measures.
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

/* The level moby array: a pointer word naming an array of fixed-stride
 * records, ended by the first record whose state byte is -1. Every game's
 * moby draw queue walks it this way (Spyro 2 func_80043858, Spyro 3
 * func_80030478: lw the pointer, then addi 0x58 / lb 0x48 per record, the
 * walk ending on a state of -1). Spyro 1 and 3 take the offsets from the
 * typed Moby view in openpete_game_structs.h, so a layout change fails
 * this compile; Spyro 2's SDK has no Moby struct yet, so its offsets are
 * the ones its draw queue reads (lw 0xC/0x10/0x14 position, lhu 0x36
 * class, lb 0x48 state). */
#if OPENPETE_GAME_SPYRO1
#define MOBY_LIST   OP_GADDR_g_LevelMobys
#define MOBY_STRIDE ((uint32_t)sizeof(Moby))
#define MOBY_POS    ((uint32_t)offsetof(Moby, m_Position))
#define MOBY_CLASS  ((uint32_t)offsetof(Moby, m_Class))
#define MOBY_STATE  ((uint32_t)offsetof(Moby, m_State))
#elif OPENPETE_GAME_SPYRO2
#define MOBY_LIST   OP_GADDR_D_80066F14   /* func_80043858: lw D_80066F14 */
#define MOBY_STRIDE 0x58u
#define MOBY_POS    0x0Cu
#define MOBY_CLASS  0x36u
#define MOBY_STATE  0x48u
#elif OPENPETE_GAME_SPYRO3
#define MOBY_LIST   OP_GADDR_D_8006C550   /* func_80030478: lw D_8006C550 */
#define MOBY_STRIDE ((uint32_t)sizeof(Moby))
#define MOBY_POS    ((uint32_t)offsetof(Moby, position))
#define MOBY_CLASS  ((uint32_t)offsetof(Moby, mobyClass))
#define MOBY_STATE  ((uint32_t)offsetof(Moby, state))
#else
#error "imgui-panel: no moby array layout for this game"
#endif
/** The array ends at the first moby whose state is -1. This bounds the
 * walk in case a mid-load array has no terminator yet. */
#define MOBY_SCAN_MAX 4096

/* ── host-side copy the section reads ──────────────────────────────── */

#define RING_N    240   /**< About eight seconds of ticks. */
#define HOVER_LOG_TICKS 30  /**< About one second of ticks. */
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

/** Hover instrument for the stats window's button. Written by the body on
 * the compose thread, read by the tick hook for the log line: an always
 * window is display-only while the overlay is closed, so a run with the
 * overlay closed must report 0 hovered out of many presents and the same
 * run with it open must report more than 0. */
static unsigned s_present_n;
static unsigned s_hover_n;
static unsigned s_hover_logged;

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

/** @brief Count the live mobys and, while a level is live, keep the ten nearest to Spyro.
 *
 * Spyro's position comes from the api's player_pose service, the game's
 * own player-object reader, so the distance needs no player layout.
 * @param cpu The tick hook's CPU, which player_pose reads through. */
static void sample_mobys(CPUState* cpu) {
    uint32_t m = op_read_u32(g_api, MOBY_LIST);
    openpete_mod_pose_t p = { .struct_size = sizeof(openpete_mod_pose_t) };
    const int have_pose = g_api->player_pose(g_self, cpu, &p) == 0;
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
        if (!have_pose) continue;
        near_insert((int)(int16_t)op_read_u16(g_api, m + MOBY_CLASS),
                    x, y, z,
                    abs_diff(x, p.x) + abs_diff(y, p.y) + abs_diff(z, p.z));
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
    sample_mobys(cpu);
    s_ring[s_ring_head] = (float)s_live_count;
    s_ring_head = (s_ring_head + 1) % RING_N;
    if (s_ring_filled < RING_N) s_ring_filled++;
    if (!s_ticks)
        g_api->log(g_self, OP_MOD_LOG_INFO, "imgui-panel: sampling mobys");
    s_ticks++;
    /* One line a second, from the tick hook because the mod API has no
     * shutdown callback to report a final tally from: the last line a run
     * emits is that run's tally. HOVER_LOG_TICKS is one second of ticks. */
    if (s_ticks - s_hover_logged >= HOVER_LOG_TICKS) {
        s_hover_logged = s_ticks;
        g_api->log(g_self, OP_MOD_LOG_INFO,
                    "imgui-panel: hovered %u/%u presents, %d live mobys",
                    s_hover_n, s_present_n, s_live_count);
    }
    g_api->base(cpu);
}

/* ── outside the section, so the guard drops the call ──────────────── */

static void on_present(const openpete_present_ctx_t* ctx) {
    (void)ctx;
    /* A present hook runs on the extracting thread, outside the engine's
     * NewFrame/Render pair. The engine drops this and names the mod once;
     * the point of the row is to see that happen. */
    ImGui_Text("imgui-panel: this call never reaches ImGui");
}

/* ── UI section: host copy in, ImGui out ───────────────────────────── */

/** Stats window geometry, in window pixels, chosen so the button sits in
 * the strip below the dev config panel (which is 540 wide at y 24 and ends
 * 96px short of the bottom edge) and stays hoverable while that panel is
 * up. The window has no title bar, so its content starts one window
 * padding in. */
#define STATS_X       16.0f
#define STATS_UP      68.0f   /**< Window top, measured up from the bottom. */
#define STATS_W      240.0f
#define STATS_H       52.0f
#define STATS_BTN_W  200.0f
#define STATS_BTN_H   36.0f

static void draw_stats_window(void) {
    ImGuiIO* io = ImGui_GetIO();
    ImDrawList* fg;
    const ImGuiWindowFlags flags =
        ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
        ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse |
        ImGuiWindowFlags_NoSavedSettings;

    /* Always, not FirstUseEver: the harness parks a cursor at a fixed
     * coordinate, so the window may not drift with a saved ini. */
    ImGui_SetNextWindowPos((ImVec2){ STATS_X, io->DisplaySize.y - STATS_UP },
                           ImGuiCond_Always);
    ImGui_SetNextWindowSize((ImVec2){ STATS_W, STATS_H }, ImGuiCond_Always);
    if (ImGui_Begin("imgui-panel stats", NULL, flags)) {
        ImGui_ButtonEx("hover me", (ImVec2){ STATS_BTN_W, STATS_BTN_H });
        s_present_n++;
        if (ImGui_IsItemHovered(0)) s_hover_n++;
    }
    ImGui_End();

    /* The foreground draw list needs no window of its own, so the marker
     * rides every present next to the stats window. */
    fg = ImGui_GetForegroundDrawList();
    ImDrawList_AddCircleEx(fg,
                           (ImVec2){ STATS_X + STATS_W + 24.0f,
                                     io->DisplaySize.y - STATS_UP + 24.0f },
                           18.0f,
                           IM_COL32((int)(s_marker_col[0] * 255.0f),
                                    (int)(s_marker_col[1] * 255.0f),
                                    (int)(s_marker_col[2] * 255.0f), 255),
                           24, 2.0f);
}

/** Plot, table and colour editor: only worth screen space while the
 * overlay is open, because with it closed nothing here can be clicked. */
static void draw_detail_window(const openpete_mod_ui_t* ui) {
    int i;

    ImGui_SetNextWindowSize((ImVec2){ 320.0f, 0.0f }, ImGuiCond_FirstUseEver);
    if (ImGui_Begin("imgui-panel details", NULL, 0)) {
        ui->text("%d live moby(s), %u tick(s) sampled", s_live_count,
                 s_ticks);

        /* The ring is written head-first and wraps, so the plot is handed
         * the head as its offset rather than being rotated into a scratch
         * copy. */
        ImGui_PlotLinesEx("live mobys", s_ring, s_ring_filled,
                          s_ring_filled == RING_N ? s_ring_head : 0,
                          NULL, 0.0f, 260.0f, (ImVec2){ 0.0f, 60.0f },
                          (int)sizeof(float));

        ImGui_ColorEdit3("marker", s_marker_col, 0);

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
                ImGui_Text("%d, %d, %d", s_near[i].x, s_near[i].y,
                           s_near[i].z);
            }
            ImGui_EndTable();
        }
        if (!s_near_n)
            ui->text_disabled("no level mobys yet");
    }
    /* The whole point of the row: with it on the window is never ended,
     * ImGui reports a recoverable error, and the engine's bracket
     * recovers the stack and refuses this section after three presents. */
    if (!s_break_end)
        ImGui_End();
}

static void ui_draw(const openpete_mod_ui_t* ui) {
    draw_stats_window();
    if (ui->overlay_open())
        draw_detail_window(ui);
    else if (s_break_end)
        /* Keep the bracket exercised with the overlay closed too: the
         * detail window is where the missing End lives. */
        draw_detail_window(ui);
}
OPENPETE_MOD_UI_SECTION_FLAGS(ui_draw, OPENPETE_MOD_UI_ALWAYS)

/**
 * @brief Attach the sampler to the running game's per-tick hook.
 *
 * Spyro 1 binds GamestateUpdate by name. Spyro 2 and 3 name no tick
 * function yet, so their arms bind the per-gameplay-tick camera update by
 * address: Spyro 2 func_8001D87C, Spyro 3 func_8001204C (the
 * UPDATE_CAMERA arm of func_80055294). Those run once per gameplay tick
 * and not on the title screen, in a pause or through a level load, so the
 * ring holds gameplay ticks only there.
 * @param api The engine table.
 * @param self This mod's handle.
 * @return The override call's result, 0 on success.
 */
static int bind_tick(const openpete_mod_api_t* api, openpete_mod_t* self) {
#if OPENPETE_GAME_SPYRO1
    return api->override_name(self, "GamestateUpdate", on_gamestate_update);
#elif OPENPETE_GAME_SPYRO2
    return api->override_addr(self, 0x8001D87Cu, on_gamestate_update);
#elif OPENPETE_GAME_SPYRO3
    return api->override_addr(self, 0x8001204Cu, on_gamestate_update);
#else
#error "imgui-panel: no tick binding for this game"
#endif
}

int openpete_mod_entry(const openpete_mod_api_t* api, openpete_mod_t* self) {
    g_api = api; g_self = self;
    if (api->api_version < 13u) return -1;

    s_break_end = api->config_bool(self, "break_end", 0);
    s_hook_call = api->config_bool(self, "hook_call", 0);

    api->ui_status(self, "open this section to drive ImGui directly");
    if (s_break_end)
        api->ui_status(self, "break_end: an ImGui_End is omitted on purpose");
    if (s_hook_call) {
        api->ui_status(self, "hook_call: a present hook calls into ImGui");
        if (api->register_present_hook(self, on_present) != 0) return -1;
    }
    return bind_tick(api, self);
}
