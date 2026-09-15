// core/runtime/fps_overlay.cpp — top-left FPS counter, drawn via direct
// OpenGL during PsyCross's post-render hook.
//
// Wired up via `g_psyx_postRenderOverlay` (PsyX_public.h). PsyX_EndScene
// invokes us after GR_StoreFrameBuffer (so the VRAM mirror snapshot is
// taken before our overlay lands) and before GR_SwapWindow (so the overlay
// is part of the presented backbuffer). At this point FB 0 is bound and
// holds the finished scene; we issue our own shader + a small VAO of solid
// rectangles to stamp a 5x7 bitmap font onto the top-left corner.
//
// State save/restore is conservative: every GL knob we touch (program, VAO,
// VBO, viewport, depth/blend/cull/scissor enables) is saved on entry and
// restored on exit so subsequent GR_SwapWindow / next frame BeginScene see
// PsyCross's pipeline state untouched.

#include "PsyX/common/glad.h"
#include "PsyX/PsyX_globals.h" /* g_windowWidth / g_windowHeight */

#include <SDL3/SDL.h>

#include <cstdint>
#include <cstdio>
#include <cstring>

/* 5x7 bitmap font. Each byte is one row (top → bottom); low 5 bits used,
 * bit 4 = leftmost pixel. We only define the glyphs we need: digits, ' ',
 * ':', and the letters 'F'/'P'/'S'. Anything else falls through to blank. */
static const uint8_t s_glyph_space[7] = {0, 0, 0, 0, 0, 0, 0};
static const uint8_t s_glyph_digit[10][7] = {
    {0x0E, 0x11, 0x13, 0x15, 0x19, 0x11, 0x0E}, /* 0 */
    {0x04, 0x0C, 0x04, 0x04, 0x04, 0x04, 0x0E}, /* 1 */
    {0x0E, 0x11, 0x01, 0x02, 0x04, 0x08, 0x1F}, /* 2 */
    {0x1F, 0x02, 0x04, 0x02, 0x01, 0x11, 0x0E}, /* 3 */
    {0x02, 0x06, 0x0A, 0x12, 0x1F, 0x02, 0x02}, /* 4 */
    {0x1F, 0x10, 0x1E, 0x01, 0x01, 0x11, 0x0E}, /* 5 */
    {0x06, 0x08, 0x10, 0x1E, 0x11, 0x11, 0x0E}, /* 6 */
    {0x1F, 0x01, 0x02, 0x04, 0x08, 0x08, 0x08}, /* 7 */
    {0x0E, 0x11, 0x11, 0x0E, 0x11, 0x11, 0x0E}, /* 8 */
    {0x0E, 0x11, 0x11, 0x0F, 0x01, 0x02, 0x0C}, /* 9 */
};
static const uint8_t s_glyph_colon[7] = {0x00, 0x04, 0x04, 0x00, 0x04, 0x04, 0x00};
static const uint8_t s_glyph_F[7] = {0x1F, 0x10, 0x10, 0x1E, 0x10, 0x10, 0x10};
static const uint8_t s_glyph_P[7] = {0x1E, 0x11, 0x11, 0x1E, 0x10, 0x10, 0x10};
static const uint8_t s_glyph_S[7] = {0x0F, 0x10, 0x10, 0x0E, 0x01, 0x01, 0x1E};

static const uint8_t *glyph_for(char c)
{
    if (c >= '0' && c <= '9')
        return s_glyph_digit[c - '0'];
    switch (c)
    {
    case ':':
        return s_glyph_colon;
    case 'F':
        return s_glyph_F;
    case 'P':
        return s_glyph_P;
    case 'S':
        return s_glyph_S;
    default:
        return s_glyph_space;
    }
}

/* GL resources, lazily initialised on first emit. */
static GLuint s_program = 0;
static GLint s_loc_screen = -1;
static GLint s_loc_color = -1;
static GLuint s_vao = 0;
static GLuint s_vbo = 0;
static int s_initialised = 0;

/* Worst case per call: 7 chars × 35 lit cells × 2 (shadow + text) × 6 verts
 * × 2 floats = 5880 floats. Round up. */
#define OPENPETE_FPS_MAX_VERT_FLOATS 8192
static GLfloat s_verts[OPENPETE_FPS_MAX_VERT_FLOATS];

static GLuint compile_shader(GLenum type, const char *src)
{
    GLuint sh = glCreateShader(type);
    glShaderSource(sh, 1, &src, NULL);
    glCompileShader(sh);
    GLint ok = 0;
    glGetShaderiv(sh, GL_COMPILE_STATUS, &ok);
    if (!ok)
    {
        char log[512] = {0};
        glGetShaderInfoLog(sh, sizeof(log), NULL, log);
        SDL_Log("openpete fps_overlay: shader compile failed: %s", log);
        glDeleteShader(sh);
        return 0;
    }
    return sh;
}

static void init_gl_resources(void)
{
    /* Pixel-space → NDC vertex shader. u_screen = (window_w, window_h). */
    static const char *vs_src =
        "#version 330 core\n"
        "layout(location=0) in vec2 a_pos;\n"
        "uniform vec2 u_screen;\n"
        "void main() {\n"
        "    vec2 ndc = vec2(a_pos.x / u_screen.x * 2.0 - 1.0,\n"
        "                    1.0 - a_pos.y / u_screen.y * 2.0);\n"
        "    gl_Position = vec4(ndc, 0.0, 1.0);\n"
        "}\n";
    static const char *fs_src =
        "#version 330 core\n"
        "uniform vec4 u_color;\n"
        "out vec4 fragColor;\n"
        "void main() { fragColor = u_color; }\n";

    GLuint vs = compile_shader(GL_VERTEX_SHADER, vs_src);
    GLuint fs = compile_shader(GL_FRAGMENT_SHADER, fs_src);
    if (!vs || !fs)
    {
        if (vs)
            glDeleteShader(vs);
        if (fs)
            glDeleteShader(fs);
        return;
    }

    s_program = glCreateProgram();
    glAttachShader(s_program, vs);
    glAttachShader(s_program, fs);
    glLinkProgram(s_program);
    glDeleteShader(vs);
    glDeleteShader(fs);

    GLint linked = 0;
    glGetProgramiv(s_program, GL_LINK_STATUS, &linked);
    if (!linked)
    {
        char log[512] = {0};
        glGetProgramInfoLog(s_program, sizeof(log), NULL, log);
        SDL_Log("openpete fps_overlay: program link failed: %s", log);
        glDeleteProgram(s_program);
        s_program = 0;
        return;
    }

    s_loc_screen = glGetUniformLocation(s_program, "u_screen");
    s_loc_color = glGetUniformLocation(s_program, "u_color");

    glGenVertexArrays(1, &s_vao);
    glGenBuffers(1, &s_vbo);

    glBindVertexArray(s_vao);
    glBindBuffer(GL_ARRAY_BUFFER, s_vbo);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 2 * sizeof(GLfloat), 0);
    glBindVertexArray(0);
    glBindBuffer(GL_ARRAY_BUFFER, 0);

    s_initialised = 1;
}

/* Append two triangles (six vertices) covering the rect [x,x+w) × [y,y+h)
 * into s_verts. Returns updated vert-float count. */
static int push_quad(int vfloat_count, float x, float y, float w, float h)
{
    if (vfloat_count + 12 > OPENPETE_FPS_MAX_VERT_FLOATS)
        return vfloat_count;
    GLfloat *p = &s_verts[vfloat_count];
    /* tri 1: TL, TR, BL */
    p[0] = x;
    p[1] = y;
    p[2] = x + w;
    p[3] = y;
    p[4] = x;
    p[5] = y + h;
    /* tri 2: TR, BR, BL */
    p[6] = x + w;
    p[7] = y;
    p[8] = x + w;
    p[9] = y + h;
    p[10] = x;
    p[11] = y + h;
    return vfloat_count + 12;
}

/* Build the vertex buffer for `text` at (origin_x, origin_y) with `pixel_size`
 * pixels per font cell, returning the float-count. Skips blank cells; emits
 * one quad per lit cell. */
static int build_text_quads(const char *text, int origin_x, int origin_y,
                            int pixel_size, int dx, int dy)
{
    const int char_advance = (5 + 1) * pixel_size;
    int vc = 0;
    for (int ci = 0; text[ci]; ci++)
    {
        const uint8_t *g = glyph_for(text[ci]);
        const int char_x = origin_x + ci * char_advance;
        for (int row = 0; row < 7; row++)
        {
            const uint8_t mask = g[row];
            if (!mask)
                continue;
            for (int col = 0; col < 5; col++)
            {
                if (!(mask & (1u << (4 - col))))
                    continue;
                vc = push_quad(vc,
                               (float)(char_x + col * pixel_size + dx),
                               (float)(origin_y + row * pixel_size + dy),
                               (float)pixel_size,
                               (float)pixel_size);
            }
        }
    }
    return vc;
}

extern "C" void openpete_fps_overlay_emit(void)
{
#if defined(__EMSCRIPTEN__)
    /* The overlay draws through PsyCross's glad-loaded desktop-GL3.3
     * function pointers, which are never initialised on the Emscripten
     * (RENDERER_OGLES / WebGL2) path — calling them traps with a WASM
     * call_indirect signature mismatch. It's a non-gameplay presentation
     * feature; no-op it here until a WebGL2/GLES3 overlay path is written.
     * (Disabling it cannot perturb gameplay-readable state.) */
    return;
#else
    /* 0.5-second rolling FPS window. Computed before we touch GL so the
     * counter still ticks even if the GL path bails out. */
    static Uint64 window_start = 0;
    static int frames_in_window = 0;
    static int fps_display = 0;

    const Uint64 freq = SDL_GetPerformanceFrequency();
    const Uint64 now = SDL_GetPerformanceCounter();
    if (window_start == 0)
        window_start = now;
    frames_in_window++;
    const Uint64 elapsed = now - window_start;
    if (elapsed >= freq / 2u)
    {
        const double secs = (double)elapsed / (double)freq;
        fps_display = (int)((double)frames_in_window / secs + 0.5);
        window_start = now;
        frames_in_window = 0;
    }

    char text[16];
    std::snprintf(text, sizeof(text), "FPS:%d", fps_display);

    /* Save every GL knob we're about to touch — including those touched by
     * lazy init — so nothing leaks back into PsyCross's pipeline. */
    GLint prev_program = 0, prev_vao = 0, prev_vbo = 0;
    GLint prev_vp[4] = {0, 0, 0, 0};
    GLboolean prev_blend, prev_depth, prev_cull, prev_scissor;
    glGetIntegerv(GL_CURRENT_PROGRAM, &prev_program);
    glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &prev_vao);
    glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &prev_vbo);
    glGetIntegerv(GL_VIEWPORT, prev_vp);
    prev_blend = glIsEnabled(GL_BLEND);
    prev_depth = glIsEnabled(GL_DEPTH_TEST);
    prev_cull = glIsEnabled(GL_CULL_FACE);
    prev_scissor = glIsEnabled(GL_SCISSOR_TEST);

    if (!s_initialised)
    {
        init_gl_resources();
    }

    if (s_initialised)
    {
        /* Layout in window pixels (host viewport, not PSX coords). */
        const int pixel_size = 1;
        const int origin_x = 8;
        const int origin_y = 8;

        glViewport(0, 0, g_windowWidth, g_windowHeight);
        glDisable(GL_DEPTH_TEST);
        glDisable(GL_CULL_FACE);
        glDisable(GL_SCISSOR_TEST);
        glDisable(GL_BLEND);

        glUseProgram(s_program);
        glUniform2f(s_loc_screen, (float)g_windowWidth, (float)g_windowHeight);
        glBindVertexArray(s_vao);
        glBindBuffer(GL_ARRAY_BUFFER, s_vbo);

        /* Two passes: shadow at +1 px, then bright text on top. */
        int vc_shadow = build_text_quads(text, origin_x, origin_y, pixel_size, 1, 1);
        if (vc_shadow > 0)
        {
            glBufferData(GL_ARRAY_BUFFER, vc_shadow * sizeof(GLfloat),
                         s_verts, GL_STREAM_DRAW);
            glUniform4f(s_loc_color, 0.0f, 0.0f, 0.0f, 1.0f);
            glDrawArrays(GL_TRIANGLES, 0, vc_shadow / 2);
        }
        int vc_text = build_text_quads(text, origin_x, origin_y, pixel_size, 0, 0);
        if (vc_text > 0)
        {
            glBufferData(GL_ARRAY_BUFFER, vc_text * sizeof(GLfloat),
                         s_verts, GL_STREAM_DRAW);
            glUniform4f(s_loc_color, 1.0f, 1.0f, 0.25f, 1.0f); /* warm yellow */
            glDrawArrays(GL_TRIANGLES, 0, vc_text / 2);
        }
    }

    /* Restore GL state. */
    glViewport(prev_vp[0], prev_vp[1], prev_vp[2], prev_vp[3]);
    glBindBuffer(GL_ARRAY_BUFFER, (GLuint)prev_vbo);
    glBindVertexArray((GLuint)prev_vao);
    glUseProgram((GLuint)prev_program);
    if (prev_blend)
        glEnable(GL_BLEND);
    else
        glDisable(GL_BLEND);
    if (prev_depth)
        glEnable(GL_DEPTH_TEST);
    else
        glDisable(GL_DEPTH_TEST);
    if (prev_cull)
        glEnable(GL_CULL_FACE);
    else
        glDisable(GL_CULL_FACE);
    if (prev_scissor)
        glEnable(GL_SCISSOR_TEST);
    else
        glDisable(GL_SCISSOR_TEST);
#endif  /* !__EMSCRIPTEN__ */
}
