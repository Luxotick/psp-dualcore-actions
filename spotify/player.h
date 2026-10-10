#ifndef SPOTIFY_PLAYER_H
#define SPOTIFY_PLAYER_H

#include <stdint.h>

/* The ME did not finish a decode in time; it may still be running, so no
 * further ME work may be dispatched and the app must reboot to exit. */
#define PLAYER_ERR_ME_TIMEOUT (-100)

/* Streams one Spotify Ogg Vorbis file: downloads from the first working CDN
 * URL on a background thread, AES-CTR decrypts as it arrives, skips the
 * 0xa7-byte Spotify header, decodes with stb_vorbis and plays it.
 * urls points at url_count NUL-terminated strings, url_stride bytes apart.
 * Returns 0 when the track played to the end. */
int spotify_play_ogg(const char *urls, unsigned int url_stride, int url_count,
                     const uint8_t key[16]);

#endif /* SPOTIFY_PLAYER_H */
