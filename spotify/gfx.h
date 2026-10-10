#ifndef SPOTIFY_GFX_H
#define SPOTIFY_GFX_H

/* Colors are ABGR8888 (as PSPotify). */
#define GFX_BLACK      0xFF000000u
#define GFX_WHITE      0xFFFFFFFFu
#define GFX_GREEN      0xFF54B91Du   /* Spotify green 1DB954 */
#define GFX_DARK_GRAY  0xFF282828u
#define GFX_MID_GRAY   0xFF404040u
#define GFX_LIGHT_GRAY 0xFFB3B3B3u
#define GFX_RED        0xFF4040E0u

#define GFX_WIDTH  480
#define GFX_HEIGHT 272

void gfx_init(void);
void gfx_clear(unsigned int color);
void gfx_rect(int x, int y, int w, int h, unsigned int color);
/* Draws UTF-8 text folded to ASCII (Turkish letters included), at most
 * max_chars cells (0 = unlimited; a cut line ends in '~'). Returns cells. */
int gfx_text(const char *text, int x, int y, unsigned int color, int max_chars);
/* ASCII text magnified `scale` times (for the pairing code). */
void gfx_text_big(const char *text, int x, int y, unsigned int color, int scale);
/* w*h ABGR pixels. */
void gfx_image(int x, int y, int w, int h, const unsigned int *pixels);
void gfx_flip(void);

#endif /* SPOTIFY_GFX_H */
