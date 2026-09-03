// openpete_mod_ui.h — custom panels in the Mods overlay.
//
// Not covered by OPENPETE_MOD_API_VERSION. This header mirrors an
// immediate-mode widget set over the engine's overlay UI library and is
// revised as that library evolves; a revision may need a source edit in
// the mod. Mods ship as source and rebuild against the staged SDK, so a
// revision costs a recompile.
//
// The stable alternatives: [[config]] rows in mod.toml, which the engine
// renders and the mod reads through api->config_*, and api->ui_status for
// text. Use this header only for a panel the schema cannot express.
//
// Registration is by exported descriptor, not an api entry: define the
// section body and put
//
//     OPENPETE_MOD_UI_SECTION(my_section_fn)
//
// at file scope in exactly one source file. The loader resolves the
// descriptor at dlopen and refuses the section, with a log line, when its
// OPENPETE_MOD_UI_ABI differs from the engine's; the mod itself still
// loads.

#ifndef OPENPETE_MOD_UI_H
#define OPENPETE_MOD_UI_H

#ifdef __cplusplus
extern "C" {
#endif

/* Annex ABI generation, checked at load. */
#define OPENPETE_MOD_UI_ABI 1u

/* Widget table, implemented by the engine's overlay. Widgets returning
 * int report 1 when the value changed this frame (checkbox, slider and
 * combo write through their pointer); button reports 1 on click.
 * tooltip() annotates the previous widget on hover. */
typedef struct openpete_mod_ui {
    void (*text)(const char* fmt, ...);          /* printf-style line   */
    void (*text_disabled)(const char* fmt, ...); /* dimmed line         */
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
    void (*tooltip)(const char* text);
    void* _reserved[8];
} openpete_mod_ui_t;

/* Section body. Runs only while the overlay is open and the mod is
 * enabled, under the render-purity guard: guest access, game calls,
 * base() and registration are refused inside it. Change host-side mod
 * state from widgets and let tick hooks consume it. */
typedef void (*openpete_mod_ui_body_fn)(const openpete_mod_ui_t* ui);

/* Registration descriptor. Define it with OPENPETE_MOD_UI_SECTION so the
 * abi field cannot go stale. */
typedef struct openpete_mod_ui_section {
    unsigned magic;        /* 'OPUI' = 0x4F505549 */
    unsigned abi;          /* == OPENPETE_MOD_UI_ABI at compile time */
    openpete_mod_ui_body_fn body;
} openpete_mod_ui_section_t;

#define OPENPETE_MOD_UI_SECTION(fn)                                       \
    const openpete_mod_ui_section_t openpete_mod_ui_section = {           \
        0x4F505549u, OPENPETE_MOD_UI_ABI, (fn) };

#ifdef __cplusplus
}
#endif

#endif /* OPENPETE_MOD_UI_H */
