#ifndef SPOTIFY_CONFIG_H
#define SPOTIFY_CONFIG_H

#include <stdint.h>
#include <stddef.h>

#define SPOTIFY_CFG_MAX_STR 1024
#define SPOTIFY_CFG_MAX_BLOB 1024

#define AUTH_TYPE_USER_PASS 0
#define AUTH_TYPE_STORED_CREDENTIALS 1
#define AUTH_TYPE_SPOTIFY_TOKEN 3

/* Sent as SystemInfo.device_id at AP login; the reusable credential blob is
 * bound to it, so login5 and clienttoken must use the same value. */
#define SPOTIFY_DEVICE_ID "sony-psp-3000-media-engine"

typedef struct {
    char username[SPOTIFY_CFG_MAX_STR];
    char token[SPOTIFY_CFG_MAX_STR];
    char password[SPOTIFY_CFG_MAX_STR];
    uint8_t blob[SPOTIFY_CFG_MAX_BLOB];
    size_t blob_len;
    /* OAuth refresh token from the device pairing flow (keymaster client). */
    char refresh_token[SPOTIFY_CFG_MAX_STR];
    /* Client id the blob was issued under; login5 only accepts a blob for
     * the client that created it. */
    char blob_client[64];
    int auth_type;
    char path[256];
} spotify_config;

/* Application directory; spotify.cfg is read and written there. */
void spotify_config_set_dir(const char *dir);
int spotify_config_load(spotify_config *cfg);
int spotify_config_save_blob(const spotify_config *cfg, const char *canonical_username, const uint8_t *blob, size_t blob_len);

#endif /* SPOTIFY_CONFIG_H */
