/* launcher_sky.glsl - the push-constant block shared by the launcher sky's
 * vertex and fragment stages (launcher_sky.vert, launcher_sky.frag). The
 * host writes it as nr_sky_push (launcher_sky_pass.c), six vec4s. */
layout(push_constant) uniform LauncherSky {
    vec4 cam;     /* yaw, pitch (radians), tan of half the horizontal FOV, tan of half the vertical FOV */
    vec4 bg;      /* rgb: the level's clear colour; w: 1 for the fill triangle, 0 for the dome */
    vec4 screen;  /* target width, height in pixels, width / height, the reveal noise's clock in seconds */
    vec4 wipe;    /* reveal centre u, v (0..1, v down), its farthest-corner radius, its soft edge (screen-height units) */
    vec4 layer;   /* reveal progress 0..1, layer opacity, 1 when the reveal mask is the opacity, grain seed */
    vec4 look;    /* vignette strength, grain amplitude, unused, unused */
} pc;
