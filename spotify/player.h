#ifndef SPOTIFY_PLAYER_H
#define SPOTIFY_PLAYER_H

#include <stdint.h>

/* The ME did not finish a decode in time; it may still be running, so no
 * further ME work may be dispatched and the app must reboot to exit. */
#define PLAYER_ERR_ME_TIMEOUT (-100)
/* Playback ended because control->stop was set. */
#define PLAYER_STOPPED 1

/* Shared between the UI thread (writes stop/paused/volume) and the player
 * thread (writes the status fields). */
typedef struct {
    volatile int stop;
    volatile int paused;
    volatile int volume;            /* 0 .. PSP_AUDIO_VOLUME_MAX */
    volatile unsigned int position_ms;
    volatile unsigned int fetched_pct;
} player_control;

/* Streams one Spotify Ogg Vorbis file: downloads from the first working CDN
 * URL on a background thread, AES-CTR decrypts as it arrives, skips the
 * 0xa7-byte Spotify header, decodes with stb_vorbis on the Media Engine and
 * plays it. urls points at url_count NUL-terminated strings, url_stride
 * bytes apart. Returns 0 at the end of the track, PLAYER_STOPPED, or < 0. */
int spotify_play_ogg(const char *urls, unsigned int url_stride, int url_count,
                     const uint8_t key[16], player_control *control);

#endif /* SPOTIFY_PLAYER_H */
