/* psx_dither.frag - engine SCENE pass (`--dither`): PS1 15-bit colour.
 *
 * The PS1 GPU shades gouraud/modulated prims at 8-bit internal precision but
 * stores an RGB555 framebuffer. To mask the 5-bit banding it adds a fixed 4x4
 * ordered matrix (-4..+3, indexed [y&3][x&3] of the framebuffer pixel) to each
 * channel before truncating, gated per prim by tpage bit 9 - which this game
 * sets on effectively every 3D prim. Both halves together are the authentic
 * look: banding where the console banded, crosshatch where it dithered, the
 * two cancelling into smooth gradients at viewing distance. (Dither without
 * the truncation is just noise - the offset exists to straddle the 8->5 bit
 * quantisation boundary.)
 *
 * The dither lattice is the PSX 512x240 FRAMEBUFFER grid, not the window
 * pixel grid: op_scene_rect/op_scene_grid map pass uv onto the virtual
 * raster (band = the (disp_h - clip_h)/2 letterbox rows outside the visible
 * 224), so the 4x4 cells stay chunky and authentic at any window size and
 * tile continuously into the Hor+ widescreen margins (negative cell indices
 * wrap via two's-complement &3). The --raster-quantize N multiplier is
 * deliberately ignored - hardware had exactly one framebuffer lattice.
 *
 * Approximation vs hardware, accepted: the console dithers+truncates at
 * EVERY prim write (blend chains quantise per write; raw-texture SPRT writes
 * truncate without dithering), while this pass quantises the final composed
 * pixel once - exact for the visible opaque surface, approximate under blend
 * stacks, and it dithers raw-sprite HUD pixels hardware would only truncate. */
#version 450

layout(location = 0) in  vec2 op_uv;
layout(location = 0) out vec4 op_color;

layout(set = 0, binding = 0) uniform sampler2D op_prev_pass;

/* Engine frame state - must stay layout-identical to op_engine_ubo in
 * openpete_shader_api.glsl (std140; only the fields this pass reads are
 * named past the header). */
layout(set = 3, binding = 0, std140) uniform op_engine_ubo {
    float op_time;
    float op_alpha;
    uint  op_tick;
    uint  op_present;
    uint  op_canonical;
    float op_aspect;
    vec2  op_resolution;
    vec2  op_window;
    vec4  op_scene_rect;   /* uv rect of the 4:3 content sub-rect */
    vec4  op_scene_grid;   /* disp_w, disp_h, clip_h, Hor+ factor */
};

/* The GPU's dither matrix (nocash GPU spec), row-major [y][x]. */
const float D[16] = float[16](-4.0,  0.0, -3.0,  1.0,
                               2.0, -2.0,  3.0, -1.0,
                              -3.0,  1.0, -4.0,  0.0,
                               3.0, -1.0,  2.0, -2.0);

void main() {
    vec3 c = texture(op_prev_pass, op_uv).rgb;

    /* pass uv -> PSX framebuffer pixel (512 across the 4:3 width; rows offset
     * by the letterband so y matches the console's own framebuffer rows). */
    float band = (op_scene_grid.y - op_scene_grid.z) * 0.5;
    float px = (op_uv.x - op_scene_rect.x) / op_scene_rect.z * op_scene_grid.x;
    float py = band + (op_uv.y - op_scene_rect.y) / op_scene_rect.w * op_scene_grid.z;
    ivec2 cell = ivec2(floor(vec2(px, py))) & 3;

    vec3 c8 = clamp(c * 255.0 + D[cell.y * 4 + cell.x], 0.0, 255.0);
    op_color = vec4(floor(c8 / 8.0) / 31.0, 1.0);
}
