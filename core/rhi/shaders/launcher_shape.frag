#version 450

/* The launcher's panels, chips and progress bar as
 * signed-distance rounded rectangles with elliptical corners
 * (core/renderer/launcher_shape_pass.c). sd is the distance in framebuffer
 * pixels from the pixel centre to the shape's edge, negative inside:
 * straight edges exactly, the corners by the ellipse's first-order
 * distance estimate k0 (k0 - 1) / k1. Coverage is clamp(0.5 - sd / aa),
 * a ramp aa pixels wide centred on the edge.
 *
 * Kinds:
 *   0 fill    the shape in col, straight alpha scaled by coverage.
 *   1 bar     the track, the fill up to fill_x (its edge ramped one
 *             pixel) and a border ring bw wide inside the edge; opaque
 *             inside, coverage at the edge.
 *   2 shadow  black of col.a, the shape moved down by off_y and blurred
 *             (1 - smoothstep(-blur / 2, blur, sd)), only outside the
 *             unmoved shape: under a translucent box it would darken the
 *             box itself. */

layout(push_constant) uniform Shape {
    vec4  rect;     /* x0, y0, x1, y1 in framebuffer pixels, y down */
    vec4  col;      /* the fill's colour, or the shadow's (its alpha) */
    vec4  track;    /* bar: track colour */
    vec4  fill;     /* bar: fill colour */
    vec4  border;   /* bar: border colour */
    vec2  r;        /* corner radii, x and y */
    float aa;       /* coverage ramp width */
    float blur;     /* shadow blur */
    float fill_x;   /* bar: the fill's right edge */
    float bw;       /* bar: border width */
    float off_y;    /* shadow: downward offset */
    int   kind;
} pc;

layout(location = 0) out vec4 outColor;

/** @brief Signed distance from a point to the shape's rounded rectangle moved down by dy.
 *
 * @param q The point, framebuffer pixels.
 * @param dy The rectangle's downward offset.
 * @return The distance, negative inside. */
float sd_shape(vec2 q, float dy)
{
    vec2 c = 0.5 * (pc.rect.xy + pc.rect.zw) + vec2(0.0, dy);
    vec2 h = 0.5 * (pc.rect.zw - pc.rect.xy);
    vec2 d = abs(q - c) - h;
    vec2 e = d + pc.r;
    if (e.x > 0.0 && e.y > 0.0) {
        float k0 = length(e / pc.r);
        float k1 = length(e / (pc.r * pc.r));
        return k0 * (k0 - 1.0) / max(k1, 1e-6);
    }
    return max(d.x, d.y);
}

void main()
{
    vec2 q = gl_FragCoord.xy;
    if (pc.kind == 2) {
        float outside = clamp(0.5 + sd_shape(q, 0.0) / pc.aa, 0.0, 1.0);
        float sd = sd_shape(q, pc.off_y);
        outColor = vec4(0.0, 0.0, 0.0, pc.col.a * (1.0 - smoothstep(-pc.blur * 0.5, pc.blur, sd)) * outside);
        return;
    }
    float sd = sd_shape(q, 0.0);
    float cov = clamp(0.5 - sd / pc.aa, 0.0, 1.0);
    if (pc.kind == 0) {
        outColor = vec4(pc.col.rgb, pc.col.a * cov);
        return;
    }
    vec3 inner = mix(pc.track.rgb, pc.fill.rgb, clamp(pc.fill_x - q.x + 0.5, 0.0, 1.0));
    float ring = clamp(0.5 + (sd + pc.bw) / pc.aa, 0.0, 1.0);
    outColor = vec4(mix(inner, pc.border.rgb, ring), cov);
}
