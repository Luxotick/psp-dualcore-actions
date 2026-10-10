/* stb_vorbis (public domain / MIT, pinned in scripts/build.py) compiled as
 * its own library: push API only, no stdio. Decoder memory comes from a
 * caller-provided stb_vorbis_alloc block, so it never calls malloc/alloca. */
#define STB_VORBIS_NO_STDIO
#define STB_VORBIS_NO_PULLDATA_API
#include "stb_vorbis.c"
