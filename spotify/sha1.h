#ifndef PSP_SPOTIFY_SHA1_H
#define PSP_SPOTIFY_SHA1_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint32_t state[5];
    uint64_t count;
    uint8_t  buffer[64];
} sha1_ctx;

void sha1_init(sha1_ctx *ctx);
void sha1_update(sha1_ctx *ctx, const uint8_t *data, size_t len);
void sha1_final(sha1_ctx *ctx, uint8_t digest[20]);

/**
 * Standard HMAC-SHA1 (RFC 2104).
 */
void hmac_sha1(const uint8_t *key, size_t key_len,
               const uint8_t *data, size_t data_len,
               uint8_t out[20]);

#ifdef __cplusplus
}
#endif

#endif /* PSP_SPOTIFY_SHA1_H */
