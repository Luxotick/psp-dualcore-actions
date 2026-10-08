#ifndef SPOTIFY_CONFIG_H
#define SPOTIFY_CONFIG_H

#include <stdint.h>
#include <stddef.h>

#define SPOTIFY_CFG_MAX_STR 1024
#define SPOTIFY_CFG_MAX_BLOB 1024

#define AUTH_TYPE_USER_PASS 0
#define AUTH_TYPE_STORED_CREDENTIALS 1
#define AUTH_TYPE_SPOTIFY_TOKEN 3

typedef struct {
    char username[SPOTIFY_CFG_MAX_STR];
    char token[SPOTIFY_CFG_MAX_STR];
    char password[SPOTIFY_CFG_MAX_STR];
    uint8_t blob[SPOTIFY_CFG_MAX_BLOB];
    size_t blob_len;
    int auth_type;
    char path[256];
} spotify_config;

int spotify_config_load(spotify_config *cfg);
int spotify_config_save_blob(const spotify_config *cfg, const char *canonical_username, const uint8_t *blob, size_t blob_len);

#endif /* SPOTIFY_CONFIG_H */
