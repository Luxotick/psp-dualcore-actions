#ifndef SPOTIFY_AUDIOKEY_H
#define SPOTIFY_AUDIOKEY_H

#include "handshake.h"
#include <stdint.h>
#include <stddef.h>

int spotify_request_audio_key(spotify_session *session,
                              const uint8_t *file_id,
                              const uint8_t *track_gid,
                              uint8_t out_aes_key[16]);

#endif /* SPOTIFY_AUDIOKEY_H */
