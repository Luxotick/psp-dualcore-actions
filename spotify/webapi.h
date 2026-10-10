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

/* Client ids: librespot's desktop/keymaster id, and the open.spotify.com web
 * player id (a stored credential may be bound to the client that created it). */
#define SPOTIFY_CLIENT_ID_KEYMASTER "65b708073fc0480ea92a077233ca87bd"
#define SPOTIFY_CLIENT_ID_WEB_PLAYER "d8a5ed958d274c2e8ee717e6a4b0971d"

int spotify_client_token(const char *client_id, char *out, size_t cap);

/* OAuth for the keymaster client. Device pairing shows a code to enter at
 * spotify.com/pair and polls until approved; refresh trades a stored
 * refresh token for a new access token (new_refresh may come back empty
 * when Spotify keeps the old one). Tokens are never logged. */
int spotify_oauth_device_pair(char *access_token, size_t access_cap,
                              char *refresh_token, size_t refresh_cap,
                              void (*show_code)(const char *user_code));
int spotify_oauth_refresh(const char *refresh_token, char *access_token, size_t access_cap,
                          char *new_refresh, size_t refresh_cap);
int spotify_login5(const char *client_id, const char *client_token, const char *username,
                   const uint8_t *stored_credential, size_t stored_credential_len,
                   char *access_token, size_t cap);
/* Returns the number of files written (alternatives are used when the track
 * itself has none), or a negative error. */
int spotify_track_files(const char *client_token, const char *access_token,
                        const char *track_id, spotify_audio_file *files, int max_files);
/* Writes up to max_urls CDN URLs, each in a url_cap slot of cdn_urls.
 * Returns the number of URLs or a negative error. */
int spotify_storage_resolve(const char *client_token, const char *access_token,
                            const uint8_t file_id[SPOTIFY_FILE_ID_LEN],
                            char *cdn_urls, size_t url_cap, int max_urls);

/* ---- Library over spclient (login5 token, protobuf) ----
 * api.spotify.com answers HTTP 429 to the shared desktop client id, so the
 * library uses the same spclient services as Spotify's own clients. */

typedef struct {
    char id[24];          /* base62; empty = the user's Liked Songs */
    char name[96];        /* UTF-8 */
    int total;
} spotify_playlist;

typedef struct {
    char id[24];          /* base62 track id */
    char name[96];
    char artist[64];
    unsigned int duration_ms;
    uint8_t cover[20];    /* album cover image file id (i.scdn.co/image/<hex>) */
    uint8_t has_cover;
} spotify_track;

/* The user's playlists (rootlist, folders flattened). */
int spotify_sp_playlists(const char *client_token, const char *access_token,
                         const char *username, spotify_playlist *out, int max);
/* Track ids of Liked Songs (playlist_id empty) or of a playlist; episodes
 * and local files are skipped. Names are filled by spotify_sp_track_details. */
int spotify_sp_tracks(const char *client_token, const char *access_token,
                      const char *username, const char *playlist_id,
                      spotify_track *out, int max);
/* Fills name/artist/duration for up to SPOTIFY_DETAILS_BATCH tracks. */
#define SPOTIFY_DETAILS_BATCH 120
int spotify_sp_track_details(const char *client_token, const char *access_token,
                             spotify_track *tracks, int count);

/* All functions in this module share one response buffer; this makes them
 * safe to call from the UI and player threads. Call once at startup. */
void spotify_webapi_init(void);

#endif /* SPOTIFY_WEBAPI_H */
