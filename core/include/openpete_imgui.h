/**
 * @file openpete_imgui.h
 * @brief The full ImGui C API inside a mod UI section.
 *
 * Not covered by #OPENPETE_MOD_API_VERSION and carrying no stability
 * promise, like openpete_mod_ui.h: the surface is ImGui's, and an ImGui
 * bump is a source-visible change. Mods ship as source and rebuild
 * against the staged SDK, so a bump costs a recompile; a prebuilt object
 * built against a different version is refused at load.
 *
 * openpete_mod_ui.h stays the simple path - eleven widgets, no opt-in. Use
 * this header for anything beyond them: windows, tables, plots, tree nodes,
 * tabs, popups, colour editors, draw lists, fonts.
 *
 * Opt in by putting
 *
 * @code
 * OPENPETE_MOD_IMGUI()
 * @endcode
 *
 * at file scope in exactly one source file of the mod. That defines the
 * table pointer every `ImGui_*` call reads and exports the descriptor the
 * loader resolves at dlopen. A mod without the macro references no table and
 * nothing is bound.
 *
 * Calls are legal only inside a mod UI section body
 * (#OPENPETE_MOD_UI_SECTION). A call from a present hook or a tick hook runs
 * outside the engine's ImGui frame, on the wrong thread; it is dropped, the
 * engine logs the function and the mod once, and the call returns a zeroed
 * value.
 *
 * The API is ImGui's own: names follow dear_bindings
 * (`ImGui::Begin` is `ImGui_Begin`, a call with every optional argument is
 * the `...Ex` form), and the reference is the comment block on each
 * declaration in `dcimgui.h` plus `ImGui_ShowDemoWindow()`.
 *
 * Lifecycle calls the engine owns - context, frame, allocator, settings,
 * input injection, capture forcing, platform IO, font-atlas mutation - are
 * absent from the table; calling one fails at the mod's compile with an
 * undefined reference. The list is at the top of openpete_imgui_macros.h.
 * The denylist protects the frame and the input path, not data reachable
 * through structs: `ImGui_GetIO()` returns a writable `ImGuiIO`, and a mod
 * can address another panel by name. Mod C runs with the game's privileges.
 */

#ifndef OPENPETE_IMGUI_H
#define OPENPETE_IMGUI_H

/* Must match the engine build: dcimgui.h guards 41 declarations on it and
 * the ImGuiIO layout depends on it. */
#ifndef IMGUI_DISABLE_OBSOLETE_FUNCTIONS
#define IMGUI_DISABLE_OBSOLETE_FUNCTIONS
#endif

#include <stdint.h>

#include "dcimgui.h"
#include "openpete_imgui_table.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief The bound table. Defined by #OPENPETE_MOD_IMGUI.
 *
 * NULL until the loader binds it, which happens before the mod's entry
 * point runs. A mod refused at load never runs and never reads it.
 */
extern const openpete_imgui_table_t* openpete_imgui_tbl;

/**
 * @brief Registration descriptor.
 *
 * Define it with #OPENPETE_MOD_IMGUI so the version, size and digest fields
 * cannot go stale. The loader compares all three against the engine's table
 * and disables the mod on a mismatch, since a mod bound to a table whose
 * slots have moved would call the wrong function on its first widget.
 */
typedef struct openpete_imgui_desc {
    uint32_t magic;             /**< 'OPIG' = 0x4F504947. */
    uint32_t imgui_version_num; /**< IMGUI_VERSION_NUM at compile time. */
    uint32_t table_size;        /**< sizeof(openpete_imgui_table_t). */
    uint8_t  table_sha256[32];  /**< Digest of the table's entry text. */
    const openpete_imgui_table_t** slot; /**< Where to write the table. */
} openpete_imgui_desc_t;

/** @brief Defines the table pointer and exports the descriptor. */
#define OPENPETE_MOD_IMGUI()                                              \
    const openpete_imgui_table_t* openpete_imgui_tbl = 0;                 \
    const openpete_imgui_desc_t openpete_imgui_desc = {                 \
        0x4F504947u, (uint32_t)IMGUI_VERSION_NUM,                         \
        (uint32_t)sizeof(openpete_imgui_table_t),                         \
        OPENPETE_IMGUI_TABLE_SHA256_INIT,                                 \
        &openpete_imgui_tbl };

#ifdef __cplusplus
}
#endif

/* After the declarations: the macros shadow the dcimgui.h names, which stay
 * in place and unused so a call reads as it does in ImGui's documentation. */
#include "openpete_imgui_macros.h"

#endif /* OPENPETE_IMGUI_H */
