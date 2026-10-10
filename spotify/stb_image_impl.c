/* stb_image (public domain / MIT, pinned with stb_vorbis) for album cover
 * JPEGs. Separate from the ME-checked stb_vorbis library: this runs on the
 * SC only and uses malloc. */
#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_JPEG
#define STBI_NO_STDIO
#define STBI_NO_LINEAR
#define STBI_NO_HDR
#include "stb_image.h"
