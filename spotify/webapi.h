#ifndef SPOTIFY_WEBAPI_H
#define SPOTIFY_WEBAPI_H

#include <stdint.h>
#include <stddef.h>

/* Spotify HTTPS services needed to play a full track:
 * clienttoken -> login5 (Bearer token) -> extended-metadata (file list)
 * -> storage-resolve (CDN URL). All bodies are protobuf. */

#define SPOTIFY_FILE_ID_LEN 20
#define SPOTIFY_GID_LEN 16

/* AudioFile.Format values from metadata.proto */
#define SPOTIFY_FMT_OGG_VORBIS_96   0
#define SPOTIFY_FMT_OGG_VORBIS_160  1
#define SPOTIFY_FMT_OGG_VORBIS_320  2
#define SPOTIFY_FMT_MP3_256         3
#define SPOTIFY_FMT_MP3_320         4
#define SPOTIFY_FMT_MP3_160         5
#define SPOTIFY_FMT_MP3_96          6

typedef struct {
    uint8_t file_id[SPOTIFY_FILE_ID_LEN];
    int format;
} spotify_audio_file;

const char *spotify_format_name(int format);

/* Base62 track id (22 chars) to 16-byte GID. Returns 0 on success. */
int spotify_base62_to_gid(const char *id, uint8_t gid[SPOTIFY_GID_LEN]);

int spotify_client_token(char *out, size_t cap);
int spotify_login5(const char *client_token, const char *username,
                   const uint8_t *stored_credential, size_t stored_credential_len,
                   char *access_token, size_t cap);
/* Returns the number of files written (alternatives are used when the track
 * itself has none), or a negative error. */
int spotify_track_files(const char *client_token, const char *access_token,
                        const char *track_id, spotify_audio_file *files, int max_files);
int spotify_storage_resolve(const char *client_token, const char *access_token,
                            const uint8_t file_id[SPOTIFY_FILE_ID_LEN],
                            char *cdn_url, size_t cap);

#endif /* SPOTIFY_WEBAPI_H */
