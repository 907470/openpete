/**
 * @file openpete_mod_ui.h
 * @brief Custom panels in the Mods overlay.
 *
 * Not covered by #OPENPETE_MOD_API_VERSION. This header mirrors an
 * immediate-mode widget set over the engine's overlay UI library and is
 * revised as that library evolves; a revision may need a source edit in
 * the mod. Mods ship as source and rebuild against the staged SDK, so a
 * revision costs a recompile.
 *
 * The stable alternatives: `[[config]]` rows in mod.toml, which the engine
 * renders and the mod reads through `api->config_*`, and `api->ui_status`
 * for text. Use this header only for a panel the schema cannot express.
 *
 * Registration is by exported descriptor, not an api entry: define the
 * section body and put
 *
 * @code
 * OPENPETE_MOD_UI_SECTION(my_section_fn)
 * @endcode
 *
 * at file scope in exactly one source file. The loader resolves the
 * descriptor at dlopen and refuses the section, with a log line, when its
 * #OPENPETE_MOD_UI_ABI differs from the engine's; the mod itself still
 * loads.
 */

#ifndef OPENPETE_MOD_UI_H
#define OPENPETE_MOD_UI_H

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Annex ABI generation, checked at load. */
#define OPENPETE_MOD_UI_ABI 2u

/**
 * @brief Section flag: run the body every present, overlay open or closed.
 *
 * Without it a body runs only inside the mod's node of the Mods panel,
 * which exists only while the overlay is open. With it the body also runs
 * from a top-level pass on every present, so a stats window or a draw-list
 * overlay stays on screen during play. See #OPENPETE_MOD_UI_SECTION_FLAGS.
 */
#define OPENPETE_MOD_UI_ALWAYS 1u

/**
 * @brief Widget table, implemented by the engine's overlay.
 *
 * Widgets returning int report 1 when the value changed this frame
 * (checkbox, slider and combo write through their pointer); button
 * reports 1 on click.
 */
typedef struct openpete_mod_ui {
    void (*text)(const char* fmt, ...);          /**< printf-style line. */
    void (*text_disabled)(const char* fmt, ...); /**< Dimmed line. */
    int  (*button)(const char* label);
    int  (*checkbox)(const char* label, int* v);
    int  (*slider_int)(const char* label, int* v, int min, int max);
    int  (*slider_float)(const char* label, float* v, float min,
                         float max);
    int  (*combo)(const char* label, int* idx, const char* const* items,
                  int count);
    int  (*input_text)(const char* label, char* buf, int cap);
    void (*separator)(void);
    void (*same_line)(void);
    void (*tooltip)(const char* text);           /**< Annotates the previous widget on hover. */
    int  (*overlay_open)(void);  /**< 1 while the dev overlay is open. An
                                  *   always body branches on it: windows
                                  *   are display-only while it reports 0. */
    void* _reserved[7];
} openpete_mod_ui_t;

/**
 * @brief Section body.
 *
 * Runs while the mod is enabled, under the render-purity guard: guest
 * access, game calls, base() and registration are refused inside it.
 * Change host-side mod state from widgets and let tick hooks consume it.
 *
 * A plain section runs inside the mod's node of the Mods panel, so only
 * while the dev overlay is open. An #OPENPETE_MOD_UI_ALWAYS section runs
 * instead from a top-level pass on every present, whatever the overlay is
 * doing, and is skipped while the in-game settings menu is open.
 *
 * An always body owns its windows: it runs at top level, not inside a
 * tree node, so every widget it emits outside an ImGui_Begin lands in
 * Dear ImGui's default debug window. With the overlay closed its windows
 * take no mouse and no keyboard - the game keeps both - and
 * openpete_mod_ui_t::overlay_open reports which state the present is in.
 */
typedef void (*openpete_mod_ui_body_fn)(const openpete_mod_ui_t* ui);

/**
 * @brief Registration descriptor.
 *
 * Define it with #OPENPETE_MOD_UI_SECTION so the abi field cannot go stale.
 */
typedef struct openpete_mod_ui_section {
    unsigned magic;        /**< 'OPUI' = 0x4F505549. */
    unsigned abi;          /**< #OPENPETE_MOD_UI_ABI at compile time. */
    openpete_mod_ui_body_fn body;
    unsigned flags;        /**< #OPENPETE_MOD_UI_ALWAYS, or 0. */
} openpete_mod_ui_section_t;

/** @brief Exports the section descriptor for @p fn, no flags. */
#define OPENPETE_MOD_UI_SECTION(fn)                                       \
    OPENPETE_MOD_UI_SECTION_FLAGS(fn, 0u)

/**
 * @brief Exports the section descriptor for @p fn with @p flags.
 *
 * @p flags is a bitwise-or of #OPENPETE_MOD_UI_ALWAYS.
 */
#define OPENPETE_MOD_UI_SECTION_FLAGS(fn, flags)                          \
    const openpete_mod_ui_section_t openpete_mod_ui_section = {           \
        0x4F505549u, OPENPETE_MOD_UI_ABI, (fn), (flags) };

#ifdef __cplusplus
}
#endif

#endif /* OPENPETE_MOD_UI_H */
