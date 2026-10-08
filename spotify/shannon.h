#ifndef PSP_SPOTIFY_SHANNON_H
#define PSP_SPOTIFY_SHANNON_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint32_t R[16];
    uint32_t CRC[16];
    uint32_t initR[16];
    uint32_t konst;
    uint32_t sbuf;
    uint32_t mbuf;
    size_t   nbuf;
} shannon_ctx;

/**
 * Initialize Shannon cipher state with a key.
 */
void shannon_init(shannon_ctx *c, const uint8_t *key, size_t keylen);

/**
 * Set IV / nonce (resets register state to initial key state).
 */
void shannon_nonce(shannon_ctx *c, const uint8_t *nonce, size_t noncelen);

/**
 * Set big-endian 4-byte nonce (used for packet sequence counter in Spotify).
 */
void shannon_nonce_u32(shannon_ctx *c, uint32_t nonce);

/**
 * Encrypt plaintext buffer in-place and accumulate MAC.
 */
void shannon_encrypt(shannon_ctx *c, uint8_t *buf, size_t len);

/**
 * Decrypt ciphertext buffer in-place and accumulate MAC.
 */
void shannon_decrypt(shannon_ctx *c, uint8_t *buf, size_t len);

/**
 * Finalize and output MAC (Spotify uses 4 bytes = 1 word).
 */
void shannon_finish(shannon_ctx *c, uint8_t *mac, size_t maclen);

#ifdef __cplusplus
}
#endif

#endif /* PSP_SPOTIFY_SHANNON_H */
