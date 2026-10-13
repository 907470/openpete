#version 450

/* Moby front-shell pre-pass fragment stage (vertex stage psx_scene.vert):
 * writes the owner of the nearest blended face of a record that draws
 * blended and opaque faces (moby_shell.glsl). The depth attachment keeps
 * the nearest face in the scene's own depth encoding; this stage writes its
 * moby identity into rgb (bits 0-7, 8-15, 16-23, the op_akey_id encoding)
 * and its present-local rank ordinal into a (0 when the moby has no ranked
 * face). The feed carries the pair in the rank attribute: x = identity,
 * y = ordinal. */

layout(location = 4) flat in vec2 vRank;

layout(location = 0) out vec4 outColor;

void main()
{
    float id = clamp(vRank.x, 0.0, 16777215.0);
    float lo = mod(id, 256.0);
    float mid = mod(floor(id / 256.0), 256.0);
    float hi = floor(id / 65536.0);
    outColor = vec4(lo, mid, hi, clamp(vRank.y, 0.0, 255.0)) / 255.0;
}
