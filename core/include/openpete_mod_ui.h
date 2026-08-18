// openpete_mod_ui.h — the mod UI ANNEX (SDK header, separately versioned).
//
// ── NO STABILITY PROMISE ────────────────────────────────────────────
// This header is NOT covered by OPENPETE_MOD_API_VERSION. It mirrors an
// immediate-mode widget set over the engine's dev-overlay UI library
// (Dear ImGui today) and is bumped freely as that library and the
// overlay evolve; a bump may require a source edit in your mod. Mods
// ship as source and rebuild automatically against the staged SDK, so
// in practice a bump costs a recompile — but the *call-level* shape of
// any widget may change between engine releases without notice.
//
// The kernel alternative that IS stable: declare settings as [[config]]
// schema rows in mod.toml and let the engine render them (values read
// back through api->config_*), and print status through api->ui_status.
// Reach for this annex only when your mod needs a genuinely custom
// panel the schema cannot express.
//
// Registration is by EXPORTED DESCRIPTOR, not an api entry (the same
// pattern as openpete_mod_stateabi): define your section body and put
//
//     OPENPETE_MOD_UI_SECTION(my_section_fn)
//
// at file scope in exactly one source file. The loader resolves the
// descriptor at dlopen and REFUSES the section (with a log line; the
// mod itself still loads) when the compiled-in OPENPETE_MOD_UI_ABI
// does not match the engine's — which protects the prebuilt-binary
// fallback path that compile-on-load cannot rebuild.

#ifndef OPENPETE_MOD_UI_H
#define OPENPETE_MOD_UI_H

#ifdef __cplusplus
extern "C" {
#endif

/* The annex ABI generation. Checked at LOAD, not only at compile. */
#define OPENPETE_MOD_UI_ABI 1u

/* ── The widget vtable ───────────────────────────────────────────────
 * Implemented by the engine's dev overlay — mods never see the
 * underlying UI library. Widgets returning int report 1 when the value
 * changed this frame (checkbox/slider/combo write through their
 * pointer), buttons report 1 on click. tooltip() annotates the
 * PREVIOUS widget on hover. */
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

/* The section body. Runs only while the overlay is open AND the mod is
 * enabled, under the render-purity guard (guest access / game calls /
 * base() / registration are refused inside — mutate host-side mod
 * state from widgets and let tick hooks consume it). */
typedef void (*openpete_mod_ui_body_fn)(const openpete_mod_ui_t* ui);

/* Loader-resolved registration descriptor. Do not define by hand — use
 * the macro below so the abi field can never go stale. */
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
