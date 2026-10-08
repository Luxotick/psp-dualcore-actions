#ifndef PSP_SPOTIFY_DH_H
#define PSP_SPOTIFY_DH_H

#include <stdint.h>
#include <stddef.h>

#define SPOTIFY_DH_KEY_SIZE 96 /* 768 bits (Oakley Group 1) */

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Generate a random private key and compute corresponding DH public key g^a mod P.
 * public_key:  96 bytes big-endian
 * private_key: 96 bytes big-endian
 */
void spotify_dh_generate_keys(uint8_t public_key[SPOTIFY_DH_KEY_SIZE],
                              uint8_t private_key[SPOTIFY_DH_KEY_SIZE]);

/**
 * Compute DH shared secret remote_key^private_key mod P.
 * shared_secret: 96 bytes big-endian
 */
void spotify_dh_compute_shared_secret(const uint8_t remote_public_key[SPOTIFY_DH_KEY_SIZE],
                                      const uint8_t private_key[SPOTIFY_DH_KEY_SIZE],
                                      uint8_t shared_secret[SPOTIFY_DH_KEY_SIZE]);

/**
 * Internal self-test verifying DH round-trip (Alice & Bob).
 * Returns 0 on success, negative on error.
 */
int spotify_dh_selftest(void);

#ifdef __cplusplus
}
#endif

#endif /* PSP_SPOTIFY_DH_H */
