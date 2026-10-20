/* core/host/stb_image_impl.c - the program's one stb_image decoder.
 *
 * PNG only (STBI_ONLY_PNG), with external linkage: texture_pack.c and
 * mod_texture.c include stb_image.h for the declarations and call
 * stbi_load / stbi_image_free here. SDL's own copy (SDL_stb.c) is
 * STB_IMAGE_STATIC and does not collide. */
#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#pragma GCC diagnostic push
// warning-ratchet: stb_image defines its JPEG-only overflow checks stbi__addints_valid and stbi__mul2shorts_valid outside the STBI_NO_JPEG guard
#pragma GCC diagnostic ignored "-Wunused-function"
#include "stb_image.h"
#pragma GCC diagnostic pop
