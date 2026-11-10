#version 450
/* Shared-vertex position readback (terrain.vert TERRAIN_READBACK): each
 * point writes the four words its vertex stage carries (position bits or
 * readback key) into the RGBA32_UINT target unchanged. */
layout(location = 2) flat in uvec4 vMat;
layout(location = 0) out uvec4 oWords;

void main()
{
    oWords = vMat;
}
